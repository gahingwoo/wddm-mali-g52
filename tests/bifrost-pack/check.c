// SPDX-License-Identifier: GPL-2.0
//
// Checks mesa-patches/0003: the explicit Bifrost pack functions must produce
// exactly what GCC's packed bitfield layout produced before (the hardware
// encoding). Built with GCC on Linux, where memcpy of the struct still gives
// that layout. Random field values, many rounds; any mismatch fails.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bifrost.h"

static uint64_t r(unsigned bits)
{
   uint64_t v = ((uint64_t)rand() << 40) ^ ((uint64_t)rand() << 20) ^ (uint64_t)rand();
   return bits >= 64 ? v : v & ((1ull << bits) - 1);
}

#define SET(s, f, w) (s).f = r(w)

int main(void)
{
   unsigned bad = 0;
   srand(1);
   for (int i = 0; i < 100000; i++) {
      struct bifrost_header h;
      memset(&h, 0, sizeof(h));
      SET(h, zero1, 5); SET(h, flush_to_zero, 2); SET(h, suppress_inf, 1); SET(h, suppress_nan, 1);
      SET(h, float_exceptions, 2); SET(h, flow_control, 3); SET(h, zero2, 1);
      SET(h, terminate_discarded_threads, 1); SET(h, next_clause_prefetch, 1);
      SET(h, staging_barrier, 1); SET(h, staging_register, 6); SET(h, dependency_wait, 8);
      SET(h, dependency_slot, 3); SET(h, message_type, 5); SET(h, next_message_type, 5);
      uint64_t a = 0; memcpy(&a, &h, sizeof(h));
      if (a != bifrost_header_pack(&h)) bad++;

      struct bifrost_regs g;
      memset(&g, 0, sizeof(g));
      SET(g, fau_idx, 8); SET(g, reg3, 6); SET(g, reg2, 6); SET(g, reg0, 5); SET(g, reg1, 6); SET(g, ctrl, 4);
      a = 0; memcpy(&a, &g, sizeof(g));
      if (a != bifrost_regs_pack(&g)) bad++;

      struct bifrost_fmt_constant c;
      memset(&c, 0, sizeof(c));
      SET(c, pos, 4); SET(c, tag, 4); SET(c, imm_1, 60); SET(c, imm_2, 60);
      uint64_t want[2] = {0, 0}, got[2];
      memcpy(want, &c, sizeof(c));
      bifrost_fmt_constant_pack(&c, got);
      if (want[0] != got[0] || want[1] != got[1]) bad++;

      struct bifrost_texture_operation t;
      memset(&t, 0, sizeof(t));
      SET(t, sampler_index_or_mode, 4); SET(t, index, 7); SET(t, immediate_indices, 1); SET(t, op, 3);
      SET(t, offset_or_bias_disable, 1); SET(t, shadow_or_clamp_disable, 1); SET(t, array, 1);
      SET(t, dimension, 2); SET(t, lod_or_fetch, 3); SET(t, zero, 1); SET(t, format, 4); SET(t, mask, 4);
      if (t.packed != bifrost_texture_operation_pack(&t)) bad++;

      struct bifrost_dual_texture_operation d;
      memset(&d, 0, sizeof(d));
      SET(d, primary_sampler_index, 2); SET(d, mode, 2); SET(d, primary_texture_index, 2);
      SET(d, secondary_sampler_index, 2); SET(d, secondary_texture_index, 2); SET(d, reserved, 1);
      SET(d, index_mode_zero, 1); SET(d, secondary_register, 6); SET(d, secondary_format, 3);
      SET(d, secondary_mask, 4); SET(d, primary_format, 3); SET(d, primary_mask, 4);
      uint32_t du = 0; memcpy(&du, &d, sizeof(d));
      if (du != bifrost_dual_texture_operation_pack(&d)) bad++;
   }
   printf("bifrost pack check: %u mismatches in 100000 rounds x 5 structs -> %s\n", bad, bad ? "FAIL" : "PASS");
   return bad != 0;
}
