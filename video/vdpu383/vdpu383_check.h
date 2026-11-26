/*
 * vdpu383_check.h - what vdpu383.sys checks before it starts a decode, and
 * what a session may ask, as plain C: driver.c, vdpu383test's null device
 * and wddm-mali-g52 (maliwddm and its host tests, a copy in
 * video/vdpu383/, identical) use the same code.
 *
 * A session is a file object another driver opened from kernel mode for
 * one process: wddm-mali-g52's maliwddm opens one per D3D device that asks
 * (MW_ESCAPE_VDEC_CALL) and passes that process's decoder calls through
 * it, so the process itself needs no handle to \\.\VDPU383. A session gets
 * only the calls below, has every address register checked against its own
 * buffers and imports, and decodes with an IOMMU directory of its own, so
 * the decoder cannot reach another session's memory whatever the register
 * file says.
 *
 * Include after vdpu383_ioctl.h. No C library, no kernel headers.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_CHECK_H
#define VDPU383_CHECK_H

#ifndef V383_INLINE
#ifdef _MSC_VER
#define V383_INLINE static __inline
#else
#define V383_INLINE static inline
#endif
#endif

/* What a lookup says about one address */
#define V383_ADDR_BAD   0       /* in no buffer of the caller */
#define V383_ADDR_OK    1
#define V383_ADDR_DEAD  2       /* in an import its exporter took back */

/* Decodes of a session wait at most this long (VDPU383_RUN_IN.timeout_ms):
 * the decode gate is held meanwhile, and the display driver's revoke waits
 * for it. */
#define V383_SESSION_TIMEOUT_MS 2000

/* The address registers (MPP's vdpu383_com.h: Vdpu383RegCommonAddr,
 * the RCB offsets, Vdpu383RegH26xAddr and the VP9/AV1 forms of 170..185). */
V383_INLINE int v383_is_addr_reg(unsigned r)
{
    return (r >= 128 && r <= 134) || (r >= 140 && r <= 160 && !(r & 1)) || (r >= 168 && r <= 185) ||
           r == 192 || (r >= 194 && r <= 210) || (r >= 216 && r <= 232);
}

typedef unsigned (*V383_ADDR_LOOKUP)(void *ctx, unsigned addr);

/* Every address register the run writes, through lookup (V383_ADDR_*; 0 is
 * always fine: no buffer). VDPU383_RUN_OK, VDPU383_RUN_BAD_ADDRESS or
 * VDPU383_RUN_REVOKED, and the register in *bad_reg. */
V383_INLINE unsigned v383_check_addrs(const unsigned *regs, const unsigned *mask, V383_ADDR_LOOKUP lookup,
                                      void *ctx, unsigned *bad_reg)
{
    unsigned r, chk;

    for (r = 0; r < V383_NUM_REGS; r++) {
        if (!(mask[r / 32] & (1u << (r % 32))) || !v383_is_addr_reg(r) || regs[r] == 0)
            continue;
        chk = lookup(ctx, regs[r]);
        if (chk != V383_ADDR_OK) {
            *bad_reg = r;
            return chk == V383_ADDR_DEAD ? VDPU383_RUN_REVOKED : VDPU383_RUN_BAD_ADDRESS;
        }
    }
    return VDPU383_RUN_OK;
}

/* The calls a session may make: no register access, no warm-up or reset
 * on demand. */
V383_INLINE int v383_session_ioctl_ok(unsigned long code)
{
    return code == IOCTL_VDPU383_GET_INFO || code == IOCTL_VDPU383_ALLOC || code == IOCTL_VDPU383_FREE ||
           code == IOCTL_VDPU383_IMPORT || code == IOCTL_VDPU383_RUN;
}

/* The sizes of a session's calls; 0 when the call is not one of them. */
V383_INLINE int v383_session_sizes(unsigned long code, unsigned *in_size, unsigned *out_size)
{
    if (code == IOCTL_VDPU383_GET_INFO) {
        *in_size = 0;
        *out_size = (unsigned)sizeof(VDPU383_INFO);
    } else if (code == IOCTL_VDPU383_ALLOC) {
        *in_size = (unsigned)sizeof(VDPU383_ALLOC_IN);
        *out_size = (unsigned)sizeof(VDPU383_ALLOC_OUT);
    } else if (code == IOCTL_VDPU383_FREE) {
        *in_size = (unsigned)sizeof(VDPU383_FREE_IN);
        *out_size = 0;
    } else if (code == IOCTL_VDPU383_IMPORT) {
        *in_size = (unsigned)sizeof(VDPU383_IMPORT_IN);
        *out_size = (unsigned)sizeof(VDPU383_IMPORT_OUT);
    } else if (code == IOCTL_VDPU383_RUN) {
        *in_size = (unsigned)sizeof(VDPU383_RUN_IN);
        *out_size = (unsigned)sizeof(VDPU383_RUN_OUT);
    } else {
        return 0;
    }
    return 1;
}

/* A session's decode: always address-checked, no register read-back (it
 * would show what another session left in registers this one does not
 * write), the link enable the UMD uses (0), a bounded wait. */
V383_INLINE void v383_session_sanitize_run(VDPU383_RUN_IN *in)
{
    in->flags &= VDPU383_RUN_RESET_ON_ERROR;
    in->link_ip_enable = 0;
    if (in->timeout_ms == 0 || in->timeout_ms > V383_SESSION_TIMEOUT_MS)
        in->timeout_ms = V383_SESSION_TIMEOUT_MS;
}

/* What a session is told about the device: versions, counters, whether the
 * IOMMU is on; no physical addresses and no register contents. */
V383_INLINE void v383_session_scrub_info(VDPU383_INFO *o)
{
    unsigned i;

    for (i = 0; i < 24; i++)
        o->link[i] = 0;
    o->mmu_dte[0] = o->mmu_dte[1] = 0;
    o->mmu_last_fault_addr[0] = o->mmu_last_fault_addr[1] = 0;
    o->sram_dma = 0;
    o->iommu_dt_dma = 0;
    o->warmup.buffer_dma = 0;
    o->last_irq = o->last_status = 0;
}

#endif /* VDPU383_CHECK_H */
