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

/* 2: buffers are mapped through the decoder's IOMMU (dma is an IO address),
 * VDPU383_RUN_MMU_FAULT, the IOMMU fields at the end of VDPU383_INFO. The
 * structures are otherwise those of 1, and a version 1 program works.
 * 3: IOCTL_VDPU383_IMPORT (another driver's pages, a display driver's
 * texture, at IO addresses of the decoder), VDPU383_RUN_REVOKED, the import
 * counters of VDPU383_INFO (in what were reserved words). A version 2
 * program works unchanged. */
#define VDPU383_INTERFACE_VERSION   3

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
#define IOCTL_VDPU383_IMPORT     CTL_CODE(VDPU383_DEVICE_TYPE, 0x808, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

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
    /* version 2; a version 1 driver returns the structure without these
     * (bytes returned), and a version 2 driver accepts the shorter buffer */
    unsigned int iommu;                 /* 1: dma addresses are IO addresses, paging on */
    unsigned int iommu_setting;         /* Device Parameters Iommu (default 1) */
    unsigned int iommu_dt_dma;          /* the directory, physical */
    unsigned int iova_pages;            /* 4 KiB pages mapped now */
    unsigned int mmu_faults;            /* page fault / bus error interrupts */
    unsigned int mmu_enables;           /* times paging was (re)programmed */
    unsigned int alloc_failures;
    /* version 3 */
    unsigned int imports;               /* imports made or refreshed */
    unsigned int revokes;               /* imports the exporter took back */
    unsigned int import_failures;
    unsigned int reserved2[2];
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

/* DMA buffers, mapped write-combined (Normal non-cacheable) into the
 * calling process. dma is the 32-bit address the decoder uses for the
 * first byte; the buffer is contiguous at dma..dma+size-1 in that space.
 * With the IOMMU on (the default, VDPU383_INFO.iommu 1) it is an IO address
 * and the pages are anywhere in DRAM; with Device Parameters Iommu 0 it is
 * physical and the buffer physically contiguous below 4 GiB. */
typedef struct VDPU383_ALLOC_IN {
    unsigned int size;          /* bytes, rounded up to 4 KiB */
} VDPU383_ALLOC_IN;

typedef struct VDPU383_ALLOC_OUT {
    unsigned int handle;
    unsigned int dma;           /* bus address the decoder uses (IOMMU off: physical) */
    unsigned int size;
    unsigned int flags;         /* VDPU383_ALLOC_IOVA; 0 from a version 1 driver */
    unsigned long long user_va; /* in the calling process */
} VDPU383_ALLOC_OUT;

#define VDPU383_ALLOC_IOVA      0x01    /* dma is an IO address */
#define VDPU383_ALLOC_IMPORTED  0x02    /* another driver's pages (IOCTL_VDPU383_IMPORT) */

typedef struct VDPU383_FREE_IN {
    unsigned int handle;
} VDPU383_FREE_IN;

/*
 * Zero copy (version 3): the pages of a display driver allocation, named by
 * the token its escape gave this process (wddm-mali-g52 maliwddm,
 * MW_ESCAPE_VDEC_EXPORT; vdpu383_import.h says how the drivers talk), at
 * a free run of the decoder's IO addresses. Only with the IOMMU on
 * (VDPU383_INFO.iommu 1); not mapped into the caller (it has the
 * allocation already). Freed with IOCTL_VDPU383_FREE like a buffer.
 *
 * When the exporter takes the pages back (the allocation is evicted or
 * destroyed), the mapping is cleared and a decode naming its addresses
 * returns VDPU383_RUN_REVOKED. handle != 0 refreshes that import from a
 * new token of the same allocation: the same IO addresses, the new pages
 * (an import that is not dead keeps its pages; the call then succeeds if
 * the token is the one it has).
 */
typedef struct VDPU383_IMPORT_IN {
    unsigned long long token;
    unsigned int pages;         /* the allocation's size in 4 KiB pages, as the exporter said */
    unsigned int handle;        /* 0: a new import */
} VDPU383_IMPORT_IN;

typedef struct VDPU383_IMPORT_OUT {
    unsigned int handle;
    unsigned int dma;           /* IO address of the allocation's first byte */
    unsigned int size;
    unsigned int flags;         /* VDPU383_ALLOC_IOVA | VDPU383_ALLOC_IMPORTED */
} VDPU383_IMPORT_OUT;

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
#define VDPU383_RUN_MMU_FAULT   5           /* IOMMU page fault or bus error; the core was
                                             * stopped (IP reset); bad_reg is the faulting
                                             * IO address, mmu_status the IOMMU status */
#define VDPU383_RUN_REVOKED     6           /* rejected before start: register bad_reg names an
                                             * import its exporter took back (refresh it) */

typedef struct VDPU383_RUN_OUT {
    unsigned int result;
    unsigned int irq;                       /* link 0x48 in the ISR */
    unsigned int status;                    /* link 0x4C in the ISR */
    unsigned int elapsed_us;
    unsigned int bad_reg;                   /* MMU_FAULT: the IO address */
    unsigned int mmu_status[2];
    unsigned int mmu_int_rawstat[2];
    unsigned int reset_status;              /* link 0x4C after an IP reset, if one was done */
    unsigned int regs[V383_NUM_REGS];       /* with VDPU383_RUN_READBACK */
} VDPU383_RUN_OUT;

#pragma pack(pop)

#endif /* VDPU383_IOCTL_H */
