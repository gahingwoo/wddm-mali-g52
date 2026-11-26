/*
 * vdec_kmd.h - the decoder's calls through maliwddm (docs/CHROMIUM-VIDEO.md,
 * C2): the UMD's video backend (video/vdec.c) hands each vdpu383.sys call
 * to maliwddm in a private escape on a D3D device of its own, and maliwddm
 * makes the call kernel to kernel in a session of vdpu383.sys it opened
 * for that device (vdpu383/vdpu383_check.h). So the process needs no
 * handle to \\.\VDPU383, and vdpu383.sys's UserAccess can stay 0 while a
 * sandboxed process (a browser's GPU process) decodes.
 *
 * Shared by video/vdec.c, maliwddm (drivers/maliwddm/vdecproxy.c) and the
 * host tests; plain C, no C library, kernel-safe. Include after
 * vdpu383/vdpu383_ioctl.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDEC_KMD_H
#define VDEC_KMD_H

#include "vdpu383/vdpu383_check.h"

/* The escape code (include/maliwddm_abi.h lists it with the others) */
#define MW_ESCAPE_VDEC_CALL     10

#define VDEC_KMD_VERSION        1
#define VDEC_KMD_MAGIC          0x33384456u     /* "VD83" */

/* Ioctl 0: no call, maliwddm's counters (VDEC_KMD_COUNTERS in Data); the
 * escape may name no device. Otherwise an IOCTL_VDPU383_* a session may
 * make (v383_session_ioctl_ok), with the sizes v383_session_sizes gives. */
#define VDEC_KMD_PROBE          0u

#define VDEC_KMD_DATA           1664

/*
 * In and out: the KMD reads Version, Ioctl, InSize, OutSize and the first
 * InSize bytes of Data, and on success writes Status (the call's NTSTATUS),
 * Returned (its bytes) and the first OutSize bytes of Data. The escape
 * fails (and nothing is written) when the call is refused before it is
 * made: path off, no device, a call or size a session may not have, too
 * many sessions.
 */
typedef struct VDEC_KMD_CALL {
    unsigned int Code;          /* MW_ESCAPE_VDEC_CALL */
    unsigned int Version;       /* VDEC_KMD_VERSION */
    unsigned int Ioctl;
    unsigned int InSize;
    unsigned int OutSize;
    unsigned int Returned;      /* out */
    int Status;                 /* out: NTSTATUS of the call */
    unsigned int Magic;         /* out: VDEC_KMD_MAGIC */
    unsigned long long Data[VDEC_KMD_DATA / 8];
} VDEC_KMD_CALL;

/* VDEC_KMD_PROBE's answer. */
typedef struct VDEC_KMD_COUNTERS {
    unsigned int On;            /* the path is on (UmdVideo and UmdVideoKmd at the adapter's start) */
    unsigned int Sessions;      /* open now */
    unsigned int SessionsMade;
    unsigned int Calls;         /* calls made */
    unsigned int Runs;          /* decodes among them */
    unsigned int Refused;       /* escapes refused */
    int LastStatus;             /* the last refusal or failed session open */
    unsigned int LastPid;       /* its process */
    unsigned int reserved[8];
} VDEC_KMD_COUNTERS;

typedef char vdec_kmd_fits[(sizeof(VDPU383_RUN_IN) <= VDEC_KMD_DATA && sizeof(VDPU383_RUN_OUT) <= VDEC_KMD_DATA &&
                            sizeof(VDPU383_INFO) <= VDEC_KMD_DATA && sizeof(VDEC_KMD_COUNTERS) <= VDEC_KMD_DATA)
                           ? 1 : -1];

/* Why the KMD refuses an escape (vdec_kmd_check) */
#define VDEC_KMD_OK             0
#define VDEC_KMD_E_SIZE         1       /* the escape is not a VDEC_KMD_CALL */
#define VDEC_KMD_E_VERSION      2
#define VDEC_KMD_E_CALL         3       /* not a call a session may make */
#define VDEC_KMD_E_LENGTH       4       /* InSize or OutSize not the call's */
#define VDEC_KMD_E_DEVICE       5       /* a call needs a device of the caller's */

/*
 * The KMD's check of one escape, on its own copy of it (so nothing changes
 * after the check): the call and its sizes, and a decode made a session's
 * (v383_session_sanitize_run; vdpu383.sys does the same again).
 */
V383_INLINE int vdec_kmd_check(VDEC_KMD_CALL *c, unsigned size, int have_device)
{
    unsigned in_size, out_size;

    if (size < sizeof(*c) || c->Code != MW_ESCAPE_VDEC_CALL)
        return VDEC_KMD_E_SIZE;
    if (c->Version != VDEC_KMD_VERSION)
        return VDEC_KMD_E_VERSION;
    if (c->Ioctl == VDEC_KMD_PROBE)
        return VDEC_KMD_OK;
    if (!v383_session_ioctl_ok(c->Ioctl) || !v383_session_sizes(c->Ioctl, &in_size, &out_size))
        return VDEC_KMD_E_CALL;
    if (c->InSize != in_size || c->OutSize != out_size)
        return VDEC_KMD_E_LENGTH;
    if (!have_device)
        return VDEC_KMD_E_DEVICE;
    if (c->Ioctl == IOCTL_VDPU383_RUN)
        v383_session_sanitize_run((VDPU383_RUN_IN *)(void *)c->Data);
    return VDEC_KMD_OK;
}

#endif /* VDEC_KMD_H */
