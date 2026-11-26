/*
 * fakekmd.h - maliwddm's MW_ESCAPE_VDEC_CALL (drivers/maliwddm/vdecproxy.c)
 * and the vdpu383.sys sessions behind it, on the host, for the UMD's video
 * backend (video/vdec.c) to talk to: the same checks (video/vdec_kmd.h,
 * video/vdpu383/vdpu383_check.h), a session per process, buffers in
 * ordinary memory, the exporter of vdecexport.c with its process check,
 * revocation and process exit.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef FAKEKMD_H
#define FAKEKMD_H

#include <stdint.h>

#define V383_IOCTL_STANDALONE 1
#include "../../video/vdpu383/vdpu383_ioctl.h"
#include "../../video/vdec_kmd.h"

/* A process with a D3D device of its own on the adapter (the escape's
 * context for vdec_set_escape). */
typedef struct fk_process {
    unsigned pid;
    int has_device;
} fk_process;

long fk_escape(void *ctx, int with_device, void *data, unsigned size);

/* MW_ESCAPE_VDEC_EXPORT: pages of an allocation of the process, a token
 * for it alone; mem is what the fake decoder writes into. */
uint64_t fk_export(const fk_process *p, unsigned pages, uint8_t *mem);
/* MwVdecGone: the allocation's pages go (eviction, destruction): the
 * import holding them is revoked, the export ends. */
void fk_gone(uint64_t token);
/* The process exits: dxgkrnl destroys its device, the session closes. */
void fk_exit(const fk_process *p);

/* What the fake has seen */
typedef struct fk_counters {
    unsigned calls, runs, refused_runs, revoked_runs, refused_escapes, sessions, imports_live;
    unsigned last_bad_reg, last_result;
} fk_counters;
void fk_get(fk_counters *c);
/* any live buffer of the process (for the tests that name it from elsewhere) */
int fk_any_buffer(const fk_process *p, uint32_t *dma, uint32_t *size, unsigned *handle);

#endif
