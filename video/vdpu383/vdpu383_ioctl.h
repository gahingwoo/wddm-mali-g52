/*
 * vdpu383_ioctl.h - the interface between vdpu383.sys and vdpu383test.exe
 *
 * A bring-up interface, not a decode API: user mode builds the whole
 * register file, the driver checks the buffer addresses in it, writes it,
 * starts the decoder and waits for the interrupt. Shared by the driver and
 * the test tool; include after <windows.h> or <ntddk.h> (or with
 * V383_IOCTL_STANDALONE for a host build).
 *
 * Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef VDPU383_IOCTL_H
#define VDPU383_IOCTL_H

#include "lib/vdpu383_h264.h"

/* \\.\VDPU383 */
#define VDPU383_DOS_NAME        L"\\DosDevices\\VDPU383"
#define VDPU383_USER_PATH       L"\\\\.\\VDPU383"

#define VDPU383_INTERFACE_VERSION   1

#ifndef CTL_CODE
#define CTL_CODE(t, f, m, a) (((t) << 16) | ((a) << 14) | ((f) << 2) | (m))
#define METHOD_BUFFERED   0
#define FILE_ANY_ACCESS   0
#define FILE_READ_ACCESS  1
#define FILE_WRITE_ACCESS 2
#endif

#define VDPU383_DEVICE_TYPE     0x8383u     /* unsigned: << 16 does not fit an int */

#define IOCTL_VDPU383_GET_INFO   CTL_CODE(VDPU383_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_VDPU383_WARMUP     CTL_CODE(VDPU383_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_VDPU383_READ_REGS  CTL_CODE(VDPU383_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_VDPU383_WRITE_REGS CTL_CODE(VDPU383_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_VDPU383_ALLOC      CTL_CODE(VDPU383_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_VDPU383_FREE       CTL_CODE(VDPU383_DEVICE_TYPE, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_VDPU383_RUN        CTL_CODE(VDPU383_DEVICE_TYPE, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_VDPU383_RESET      CTL_CODE(VDPU383_DEVICE_TYPE, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#pragma pack(push, 8)

/* Result of the RK3576 warm-up decode (mpp_hack_rk3576.c). */
typedef struct VDPU383_WARMUP_RESULT {
    unsigned int ran;           /* 0: not run (no buffer) */
    unsigned int timed_out;     /* no irq bit within 500 us */
    unsigned int irq;           /* link 0x48 when it fired (or at the timeout) */
    unsigned int status;        /* link 0x4C */
    unsigned int polls;         /* 1 us polls until irq */
    unsigned int buffer_dma;
} VDPU383_WARMUP_RESULT;

typedef struct VDPU383_INFO {
    unsigned int interface_version;
    unsigned int reg_version;           /* function register 0 */
    unsigned int link[24];              /* link registers 0x00..0x5C */
    unsigned int mmu_status[2];         /* 0x804, 0x904 */
    unsigned int mmu_dte[2];            /* 0x800, 0x900 */
    unsigned int mmu_int_rawstat[2];
    unsigned int sram_present;
    unsigned int sram_dma;
    unsigned int sram_size;
    unsigned int irq_count;             /* decoder interrupts seen */
    unsigned int mmu_irq_count;
    unsigned int last_irq, last_status;
    unsigned int mmu_last_status[2], mmu_last_fault_addr[2];
    unsigned int runs, timeouts;
    VDPU383_WARMUP_RESULT warmup;       /* the one run at D0 entry */
} VDPU383_INFO;

/* Register access: offsets are bytes into the 4 KiB block, 4-aligned. */
#define VDPU383_MAX_REG_OPS 1024
typedef struct VDPU383_REG_OP {
    unsigned int offset;
    unsigned int value;
} VDPU383_REG_OP;

typedef struct VDPU383_READ_REGS_IN {
    unsigned int offset;        /* first byte offset */
    unsigned int count;         /* words */
} VDPU383_READ_REGS_IN;
/* output: count words */

typedef struct VDPU383_WRITE_REGS_IN {
    unsigned int count;
    VDPU383_REG_OP ops[1];      /* count entries */
} VDPU383_WRITE_REGS_IN;

/* DMA buffers: physically contiguous, below 4 GiB, mapped write-combined
 * (Normal non-cacheable) into the calling process. */
typedef struct VDPU383_ALLOC_IN {
    unsigned int size;          /* bytes, rounded up to 4 KiB */
} VDPU383_ALLOC_IN;

typedef struct VDPU383_ALLOC_OUT {
    unsigned int handle;
    unsigned int dma;           /* bus address the decoder uses (IOMMU off: physical) */
    unsigned int size;
    unsigned int reserved;
    unsigned long long user_va; /* in the calling process */
} VDPU383_ALLOC_OUT;

typedef struct VDPU383_FREE_IN {
    unsigned int handle;
} VDPU383_FREE_IN;

/* One decode. */
#define VDPU383_RUN_NO_ADDR_CHECK   0x01    /* allow addresses outside this handle's buffers */
#define VDPU383_RUN_READBACK        0x02    /* return all 384 function registers afterwards */
#define VDPU383_RUN_RESET_ON_ERROR  0x04    /* IP reset when the status has an error bit */

typedef struct VDPU383_RUN_IN {
    unsigned int flags;
    unsigned int timeout_ms;                /* 0: 1000 */
    unsigned int link_ip_timeout;           /* link 0x54; mainline: register 13 */
    unsigned int link_ip_enable;            /* link 0x58; mainline 0, vendor 0x01000000 */
    unsigned int regs[V383_NUM_REGS];
    unsigned int write_mask[V383_MASK_WORDS];
} VDPU383_RUN_IN;

#define VDPU383_RUN_OK          0
#define VDPU383_RUN_TIMEOUT     1
#define VDPU383_RUN_HW_ERROR    2           /* status has an error bit */
#define VDPU383_RUN_BAD_ADDRESS 3           /* rejected before start; reg in bad_reg */
#define VDPU383_RUN_BUSY        4

typedef struct VDPU383_RUN_OUT {
    unsigned int result;
    unsigned int irq;                       /* link 0x48 in the ISR */
    unsigned int status;                    /* link 0x4C in the ISR */
    unsigned int elapsed_us;
    unsigned int bad_reg;
    unsigned int mmu_status[2];
    unsigned int mmu_int_rawstat[2];
    unsigned int reset_status;              /* link 0x4C after an IP reset, if one was done */
    unsigned int regs[V383_NUM_REGS];       /* with VDPU383_RUN_READBACK */
} VDPU383_RUN_OUT;

#pragma pack(pop)

#endif /* VDPU383_IOCTL_H */
