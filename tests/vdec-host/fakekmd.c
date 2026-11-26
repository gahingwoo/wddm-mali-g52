/*
 * fakekmd.c - see fakekmd.h.
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fakekmd.h"

#define ST_SUCCESS              0L
#define ST_INVALID_HANDLE       ((long)0xC0000008L)
#define ST_INVALID_PARAMETER    ((long)0xC000000DL)
#define ST_NO_MEMORY            ((long)0xC0000017L)
#define ST_ACCESS_DENIED        ((long)0xC0000022L)
#define ST_NOT_SUPPORTED        ((long)0xC00000BBL)
#define ST_NOT_FOUND            ((long)0xC0000225L)
#define ST_SHARING_VIOLATION    ((long)0xC0000043L)

#define FK_BUFS 1024
#define FK_EXPORTS 256

typedef struct fk_buf {
    int used, import, dead;
    unsigned pid, handle;
    uint32_t dma, size;
    uint8_t *cpu;
    uint64_t token;
} fk_buf;

typedef struct fk_export_rec {
    int used;
    unsigned pid, pages, import_handle;     /* 0: no import holds it */
    uint64_t token;
    uint8_t *mem;
} fk_export_rec;

static fk_buf bufs[FK_BUFS];
static fk_export_rec exps[FK_EXPORTS];
static unsigned next_handle, sessions_of[64];
static uint32_t next_dma = 0x01000000u;
static uint64_t next_token = 0x9000;
static fk_counters cnt;

static unsigned cur_pid;                /* the session a lookup is for */

/* vdpu383.sys V383AddrCheck for a session: its own buffers and imports */
static unsigned lookup(void *ctx, unsigned addr)
{
    unsigned i;
    (void)ctx;
    for (i = 0; i < FK_BUFS; i++) {
        const fk_buf *b = &bufs[i];
        if (b->used && b->pid == cur_pid && addr >= b->dma && (uint64_t)addr < (uint64_t)b->dma + b->size)
            return b->dead ? V383_ADDR_DEAD : V383_ADDR_OK;
    }
    return V383_ADDR_BAD;
}

static fk_buf *slot(void)
{
    unsigned i;
    for (i = 0; i < FK_BUFS; i++)
        if (!bufs[i].used) {
            memset(&bufs[i], 0, sizeof(bufs[i]));
            bufs[i].used = 1;
            return &bufs[i];
        }
    return NULL;
}

static fk_buf *find(unsigned pid, unsigned handle)
{
    unsigned i;
    for (i = 0; i < FK_BUFS; i++)
        if (bufs[i].used && bufs[i].pid == pid && bufs[i].handle == handle)
            return &bufs[i];
    return NULL;
}

static fk_export_rec *find_export(uint64_t token)
{
    unsigned i;
    for (i = 0; i < FK_EXPORTS; i++)
        if (exps[i].used && exps[i].token == token)
            return &exps[i];
    return NULL;
}

static void release_import(fk_buf *b)
{
    fk_export_rec *x = find_export(b->token);
    if (x && x->import_handle == b->handle)
        x->import_handle = 0;
}

/* One call of a vdpu383.sys session (driver.c) for process pid. */
static long session_call(unsigned pid, VDEC_KMD_CALL *c)
{
    void *d = c->Data;

    cur_pid = pid;
    c->Returned = 0;
    switch (c->Ioctl) {
    case IOCTL_VDPU383_GET_INFO: {
        VDPU383_INFO *o = (VDPU383_INFO *)d;
        memset(o, 0, sizeof(*o));
        o->interface_version = VDPU383_INTERFACE_VERSION;
        o->iommu = 1;
        o->runs = cnt.runs;
        o->session_runs = cnt.runs;
        o->sessions = cnt.sessions;
        v383_session_scrub_info(o);
        c->Returned = sizeof(*o);
        return ST_SUCCESS;
    }
    case IOCTL_VDPU383_ALLOC: {
        VDPU383_ALLOC_IN in = *(VDPU383_ALLOC_IN *)d;
        VDPU383_ALLOC_OUT *o = (VDPU383_ALLOC_OUT *)d;
        fk_buf *b;
        uint32_t size = (in.size + 4095) & ~4095u;
        if (!size || size > (256u << 20) || !(b = slot()))
            return ST_INVALID_PARAMETER;
        b->cpu = calloc(1, size);
        if (!b->cpu) {
            b->used = 0;
            return ST_NO_MEMORY;
        }
        b->pid = pid;
        b->handle = ++next_handle;
        b->dma = next_dma;
        b->size = size;
        next_dma += size + 4096;
        memset(o, 0, sizeof(*o));
        o->handle = b->handle;
        o->dma = b->dma;
        o->size = size;
        o->flags = VDPU383_ALLOC_IOVA;
        o->user_va = (unsigned long long)(uintptr_t)b->cpu;
        c->Returned = sizeof(*o);
        return ST_SUCCESS;
    }
    case IOCTL_VDPU383_FREE: {
        fk_buf *b = find(pid, ((VDPU383_FREE_IN *)d)->handle);
        if (!b)
            return ST_INVALID_HANDLE;
        if (b->import)
            release_import(b);
        else
            free(b->cpu);
        b->used = 0;
        return ST_SUCCESS;
    }
    case IOCTL_VDPU383_IMPORT: {
        VDPU383_IMPORT_IN in = *(VDPU383_IMPORT_IN *)d;
        VDPU383_IMPORT_OUT *o = (VDPU383_IMPORT_OUT *)d;
        fk_export_rec *x = find_export(in.token);
        fk_buf *b;
        /* maliwddm VdecCallback: no such export, another process's token,
         * imported already by another import, too few pages */
        if (!x || x->pid != pid)
            return ST_NOT_FOUND;
        if (x->import_handle && x->import_handle != in.handle)
            return ST_SHARING_VIOLATION;
        if (in.pages != x->pages)
            return ST_INVALID_PARAMETER;
        if (in.handle) {
            b = find(pid, in.handle);
            if (!b || !b->import || b->size != in.pages * 4096u)
                return ST_INVALID_HANDLE;
            b->dead = 0;
        } else {
            if (!(b = slot()))
                return ST_NO_MEMORY;
            b->pid = pid;
            b->import = 1;
            b->handle = ++next_handle;
            b->dma = next_dma;
            b->size = in.pages * 4096u;
            next_dma += b->size + 4096;
        }
        b->token = in.token;
        b->cpu = x->mem;
        x->import_handle = b->handle;
        memset(o, 0, sizeof(*o));
        o->handle = b->handle;
        o->dma = b->dma;
        o->size = b->size;
        o->flags = VDPU383_ALLOC_IOVA | VDPU383_ALLOC_IMPORTED;
        c->Returned = sizeof(*o);
        return ST_SUCCESS;
    }
    case IOCTL_VDPU383_RUN: {
        VDPU383_RUN_IN *in = (VDPU383_RUN_IN *)malloc(sizeof(*in));
        VDPU383_RUN_OUT *o = (VDPU383_RUN_OUT *)d;
        unsigned bad = 0, r;
        if (!in)
            return ST_NO_MEMORY;
        memcpy(in, d, sizeof(*in));
        /* driver.c V383RunGated: the session's rules again, the check */
        v383_session_sanitize_run(in);
        r = v383_check_addrs(in->regs, in->write_mask, lookup, NULL, &bad);
        free(in);
        memset(o, 0, sizeof(*o));
        o->result = r;
        o->bad_reg = bad;
        o->status = r == VDPU383_RUN_OK ? V383_STA_FRAME_RDY : 0;
        o->elapsed_us = r == VDPU383_RUN_OK ? 2200 : 0;
        cnt.last_result = r;
        cnt.last_bad_reg = bad;
        if (r == VDPU383_RUN_OK)
            cnt.runs++;
        else if (r == VDPU383_RUN_REVOKED)
            cnt.revoked_runs++;
        else
            cnt.refused_runs++;
        c->Returned = sizeof(*o);
        return ST_SUCCESS;
    }
    default:
        return ST_ACCESS_DENIED;
    }
}

/* maliwddm MwEscapeVdecCall */
long fk_escape(void *ctx, int with_device, void *data, unsigned size)
{
    const fk_process *p = (const fk_process *)ctx;
    VDEC_KMD_CALL *c, *out = (VDEC_KMD_CALL *)data;
    long st;
    int why;

    if (size < sizeof(VDEC_KMD_CALL)) {
        cnt.refused_escapes++;
        return ST_INVALID_PARAMETER;
    }
    c = (VDEC_KMD_CALL *)malloc(sizeof(*c));
    if (!c)
        return ST_NO_MEMORY;
    memcpy(c, data, sizeof(*c));
    why = vdec_kmd_check(c, size, with_device && p->has_device);
    if (why == VDEC_KMD_OK && c->Ioctl == VDEC_KMD_PROBE) {
        VDEC_KMD_COUNTERS k;
        memset(&k, 0, sizeof(k));
        k.On = 1;
        k.Sessions = cnt.sessions;
        k.Runs = cnt.runs;
        k.Calls = cnt.calls;
        out->Status = 0;
        out->Returned = sizeof(k);
        out->Magic = VDEC_KMD_MAGIC;
        memcpy(out->Data, &k, sizeof(k));
        free(c);
        return ST_SUCCESS;
    }
    if (why != VDEC_KMD_OK) {
        cnt.refused_escapes++;
        free(c);
        return why == VDEC_KMD_E_CALL ? ST_ACCESS_DENIED : ST_INVALID_PARAMETER;
    }
    if (p->pid < 64 && !sessions_of[p->pid]) {
        sessions_of[p->pid] = 1;
        cnt.sessions++;
    }
    st = session_call(p->pid, c);
    cnt.calls++;
    out->Status = (int)st;
    out->Returned = st == ST_SUCCESS && c->Returned <= c->OutSize ? c->Returned : 0;
    out->Magic = VDEC_KMD_MAGIC;
    if (out->Returned)
        memcpy(out->Data, c->Data, out->Returned);
    free(c);
    return ST_SUCCESS;
}

uint64_t fk_export(const fk_process *p, unsigned pages, uint8_t *mem)
{
    unsigned i;
    for (i = 0; i < FK_EXPORTS; i++)
        if (!exps[i].used) {
            memset(&exps[i], 0, sizeof(exps[i]));
            exps[i].used = 1;
            exps[i].pid = p->pid;
            exps[i].pages = pages;
            exps[i].mem = mem;
            exps[i].token = ++next_token;
            return exps[i].token;
        }
    return 0;
}

void fk_gone(uint64_t token)
{
    fk_export_rec *x = find_export(token);
    unsigned i;

    if (!x)
        return;
    if (x->import_handle)
        for (i = 0; i < FK_BUFS; i++)
            if (bufs[i].used && bufs[i].import && bufs[i].handle == x->import_handle)
                bufs[i].dead = 1;   /* V383ImportRevoke: entries cleared, addresses kept */
    x->used = 0;
}

void fk_exit(const fk_process *p)
{
    unsigned i;

    for (i = 0; i < FK_BUFS; i++)
        if (bufs[i].used && bufs[i].pid == p->pid) {
            if (bufs[i].import)
                release_import(&bufs[i]);
            else
                free(bufs[i].cpu);
            bufs[i].used = 0;
        }
    if (p->pid < 64 && sessions_of[p->pid]) {
        sessions_of[p->pid] = 0;
        cnt.sessions--;
    }
}

void fk_get(fk_counters *c)
{
    unsigned i;
    *c = cnt;
    c->imports_live = 0;
    for (i = 0; i < FK_BUFS; i++)
        c->imports_live += bufs[i].used && bufs[i].import;
}

int fk_any_buffer(const fk_process *p, uint32_t *dma, uint32_t *size, unsigned *handle)
{
    unsigned i;
    for (i = 0; i < FK_BUFS; i++)
        if (bufs[i].used && bufs[i].pid == p->pid && !bufs[i].import) {
            *dma = bufs[i].dma;
            *size = bufs[i].size;
            *handle = bufs[i].handle;
            return 1;
        }
    return 0;
}
