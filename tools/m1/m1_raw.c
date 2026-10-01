// SPDX-License-Identifier: GPL-2.0
/*
 * M1, step 2: run one WRITE_VALUE job on the RK3576 Mali-G52 with no GPU
 * driver at all, from Linux userspace through /dev/mem. Every register value
 * here is what the Windows driver will have to write.
 *
 * Preconditions: boot with "modprobe.blacklist=panfrost regulator_ignore_unused".
 * Do not unbind Panfrost at runtime: on this kernel that panics. And without
 * regulator_ignore_unused Linux switches vdd_gpu_s0 (RK806 DCDC5) off 30 s
 * after boot because no driver claims it; PD_GPU then powers "on" with no
 * supply and its bus never leaves idle (PMU ACK0 bit 0 stays set).
 *
 * Reference values were read back from Panfrost on this board (2026-10-01):
 *   AS0 TRANSTAB = pgd | 0x7, MEMATTR = 0x888d88, TRANSCFG = 0 (legacy)
 *   PMU PWR_CON1 bit 9 = 0 powers PD_GPU; REQ0 bit 0 = 0 releases bus idle
 *
 * Build: gcc -O2 -Wall -o m1_raw m1_raw.c
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PMU_BASE   0x27380000UL
#define CRU_BASE   0x27200000UL
#define GPU_BASE   0x27800000UL

/* PMU (mainline pm-domains.c, rk3576_pmu and RK3576_PD_GPU) */
#define PMU_PWR_CON1     0x214   /* bit 9: PD_GPU off */
#define PMU_STATUS0      0x230   /* bit 25: PD_GPU off */
#define PMU_REQ0         0x110   /* bit 0: GPU bus idle request */
#define PMU_ACK0         0x120
#define PMU_IDLE0        0x128
#define PMU_REPAIR0      0x570   /* bit 25: PD_GPU memory repair done */
#define PMU_CLK_UNGATE   0x140   /* bit 0: PD_GPU, held open during the transition */

/* CRU (clk-rk3576.c) */
#define CRU_CLKSEL165    (0x300 + 165 * 4)   /* clk_gpu_src_pre: mux [7:5], div [4:0] */
#define CRU_GATE69       (0x800 + 69 * 4)    /* bit 1 src_pre, bit 3 clk_gpu, bit 8 pclk_gpu_root */

/* Mali (panfrost_regs.h) */
#define GPU_ID           0x000
#define GPU_INT_RAWSTAT  0x020
#define GPU_INT_CLEAR    0x024
#define GPU_CMD          0x030
#define   GPU_CMD_SOFT_RESET  1
#define   GPU_IRQ_RESET_COMPLETED (1u << 8)
#define SHADER_PRESENT   0x100
#define SHADER_READY     0x140
#define TILER_READY      0x150
#define L2_READY         0x160
#define SHADER_PWRON     0x180
#define TILER_PWRON      0x190
#define L2_PWRON         0x1a0
#define JOB_INT_RAWSTAT  0x1000
#define JOB_INT_CLEAR    0x1004
#define JS0              0x1800
#define   JS_STATUS          0x24
#define   JS_HEAD_NEXT_LO    0x40
#define   JS_HEAD_NEXT_HI    0x44
#define   JS_AFFINITY_NEXT_LO 0x50
#define   JS_AFFINITY_NEXT_HI 0x54
#define   JS_CONFIG_NEXT     0x58
#define   JS_COMMAND_NEXT    0x60
#define MMU_INT_RAWSTAT  0x2000
#define MMU_INT_CLEAR    0x2004
#define AS0              0x2400
#define   AS_TRANSTAB_LO     0x00
#define   AS_TRANSTAB_HI     0x04
#define   AS_MEMATTR_LO      0x08
#define   AS_MEMATTR_HI      0x0c
#define   AS_COMMAND         0x18
#define   AS_FAULTSTATUS     0x1c
#define   AS_FAULTADDR_LO    0x20
#define   AS_FAULTADDR_HI    0x24
#define   AS_STATUS          0x28
#define   AS_TRANSCFG_LO     0x30
#define   AS_TRANSCFG_HI     0x34
#define AS_CMD_UPDATE    1
#define AS_CMD_FLUSH_MEM 5

#define JS_CONFIG (0u /* AS0 */ | (8u << 16) | (3u << 8) | (3u << 12))

#define GPU_VA           0x2000000ULL
#define TARGET_OFFSET    0x100
#define MAGIC            0xC0FFEE42u

static volatile uint32_t *pmu, *cru, *gpu;

static volatile uint32_t *map_mmio(int fd, unsigned long pa, size_t len)
{
	void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pa);
	if (p == MAP_FAILED) { perror("mmap mmio"); exit(1); }
	return p;
}
#define R(base, off)     ((base)[(off) / 4])
#define W(base, off, v)  ((base)[(off) / 4] = (v))

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* Poll until (reg & mask) == want or 100 ms pass. */
static int poll(volatile uint32_t *base, unsigned off, uint32_t mask, uint32_t want, const char *what)
{
	double end = now() + 0.1;
	uint32_t v;
	do {
		v = R(base, off);
		if ((v & mask) == want)
			return 0;
	} while (now() < end);
	printf("TIMEOUT %s: reg 0x%x = 0x%08x (mask 0x%08x want 0x%08x)\n", what, off, v, mask, want);
	return -1;
}

/* EL0 cache maintenance; Linux sets SCTLR_EL1.UCI. */
static void dcache(void *p, size_t len, int invalidate)
{
	for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + len; a += 64) {
		if (invalidate)
			__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
		else
			__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t phys_of(void *va)
{
	static int pm = -1;
	if (pm < 0 && (pm = open("/proc/self/pagemap", O_RDONLY)) < 0) { perror("pagemap"); exit(1); }
	uint64_t e;
	if (pread(pm, &e, 8, ((uintptr_t)va / 4096) * 8) != 8) { perror("pread pagemap"); exit(1); }
	if (!(e & (1ULL << 63)) || !(e & ((1ULL << 55) - 1))) { printf("page not present / no PFN (root?)\n"); exit(1); }
	return (e & ((1ULL << 55) - 1)) * 4096;
}

int main(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) { perror("/dev/mem"); return 1; }
	pmu = map_mmio(fd, PMU_BASE, 0x1000);
	cru = map_mmio(fd, CRU_BASE, 0x1000);

	/* 1. Clock and power domain. */
	printf("before: PWR_CON1=%08x STATUS0=%08x REQ0=%08x ACK0=%08x CLKSEL165=%08x GATE69=%08x\n",
	       R(pmu, PMU_PWR_CON1), R(pmu, PMU_STATUS0), R(pmu, PMU_REQ0), R(pmu, PMU_ACK0),
	       R(cru, CRU_CLKSEL165), R(cru, CRU_GATE69));
	/*
	 * rockchip_pd_power(): every clock of the domain (CLK_GPU and
	 * PCLK_GPU_ROOT) and the PMU clock ungate must be on while the domain
	 * changes state and leaves bus idle; without PCLK_GPU_ROOT the idle
	 * acknowledge never clears. Linux gates them again afterwards, which is
	 * why a read-back of a running GPU shows PCLK_GPU_ROOT gated.
	 */
	/* GPLL / 6 = 198 MHz, what Linux programs (DT assigned-clock-rates).
	 * The reset default, 0x40, selects AUPLL. */
	W(cru, CRU_CLKSEL165, (0xFFu << 16) | 0x05);
	W(cru, CRU_GATE69, (0x10Au << 16) | 0);          /* ungate bits 1, 3, 8 */
	W(pmu, PMU_CLK_UNGATE, (1u << 16) | 1u);
	W(pmu, PMU_PWR_CON1, (1u << (9 + 16)) | 0);      /* PD_GPU on */
	if (poll(pmu, PMU_STATUS0, 1u << 25, 0, "PD_GPU power status")) return 1;
	if (poll(pmu, PMU_REPAIR0, 1u << 25, 1u << 25, "PD_GPU repair")) return 1;
	W(pmu, PMU_REQ0, (1u << 16) | 0);                /* release bus idle */
	if (poll(pmu, PMU_ACK0, 1u, 0, "idle ack")) return 1;
	if (poll(pmu, PMU_IDLE0, 1u, 0, "idle status")) return 1;
	W(pmu, PMU_CLK_UNGATE, (1u << 16) | 0);
	printf("after:  PWR_CON1=%08x STATUS0=%08x REQ0=%08x ACK0=%08x IDLE0=%08x REPAIR0=%08x\n",
	       R(pmu, PMU_PWR_CON1), R(pmu, PMU_STATUS0), R(pmu, PMU_REQ0), R(pmu, PMU_ACK0),
	       R(pmu, PMU_IDLE0), R(pmu, PMU_REPAIR0));

	/* Only now is it safe to touch the GPU's own registers. */
	gpu = map_mmio(fd, GPU_BASE, 0x4000);
	printf("GPU_ID=%08x SHADER_PRESENT=%08x\n", R(gpu, GPU_ID), R(gpu, SHADER_PRESENT));
	if ((R(gpu, GPU_ID) >> 16) != 0x7402) { printf("not a G52, stopping\n"); return 1; }

	/* 2. Soft reset, then power the L2, tiler and shader cores. */
	W(gpu, GPU_INT_CLEAR, 0xffffffff);
	W(gpu, GPU_CMD, GPU_CMD_SOFT_RESET);
	if (poll(gpu, GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, "soft reset")) return 1;
	W(gpu, GPU_INT_CLEAR, 0xffffffff);
	uint32_t cores = R(gpu, SHADER_PRESENT);
	W(gpu, L2_PWRON, 1);
	if (poll(gpu, L2_READY, 1, 1, "L2 ready")) return 1;
	W(gpu, TILER_PWRON, 1);
	W(gpu, SHADER_PWRON, cores);
	if (poll(gpu, TILER_READY, 1, 1, "tiler ready")) return 1;
	if (poll(gpu, SHADER_READY, cores, cores, "shaders ready")) return 1;
	printf("powered: L2_READY=%08x TILER_READY=%08x SHADER_READY=%08x\n",
	       R(gpu, L2_READY), R(gpu, TILER_READY), R(gpu, SHADER_READY));

	/* 3. Five pinned pages: L0..L3 tables and one data page. */
	uint8_t *pages;
	if (posix_memalign((void **)&pages, 4096, 5 * 4096)) { perror("alloc"); return 1; }
	memset(pages, 0, 5 * 4096);
	if (mlock(pages, 5 * 4096)) { perror("mlock"); return 1; }
	uint64_t *l0 = (uint64_t *)(pages), *l1 = (uint64_t *)(pages + 4096),
		 *l2 = (uint64_t *)(pages + 8192), *l3 = (uint64_t *)(pages + 12288);
	uint32_t *data = (uint32_t *)(pages + 16384);
	uint64_t pa_l0 = phys_of(l0), pa_l1 = phys_of(l1), pa_l2 = phys_of(l2),
		 pa_l3 = phys_of(l3), pa_data = phys_of(data);
	printf("pages: l0=%llx l1=%llx l2=%llx l3=%llx data=%llx\n",
	       (unsigned long long)pa_l0, (unsigned long long)pa_l1, (unsigned long long)pa_l2,
	       (unsigned long long)pa_l3, (unsigned long long)pa_data);

	/* Mali LPAE: tables type 3; leaf type 1 (BLOCK, even at L3), no AF,
	 * read 0x40 | write 0x80, ATTRINDX 1 (0x8D) 0x4, outer shareable 0x200. */
	l0[(GPU_VA >> 39) & 0x1ff] = pa_l1 | 3;
	l1[(GPU_VA >> 30) & 0x1ff] = pa_l2 | 3;
	l2[(GPU_VA >> 21) & 0x1ff] = pa_l3 | 3;
	l3[(GPU_VA >> 12) & 0x1ff] = pa_data | 0x2C5;

	/* The job, at GPU_VA; the target word at GPU_VA + 0x100. */
	uint64_t target = GPU_VA + TARGET_OFFSET;
	data[4] = 1u | (2u << 1) | (1u << 16);   /* is_64b, WRITE_VALUE, index 1 */
	data[8] = (uint32_t)target;
	data[9] = (uint32_t)(target >> 32);
	data[10] = 6;                            /* immediate 32 */
	data[12] = MAGIC;
	dcache(pages, 5 * 4096, 0);

	/* 4. Address space 0. */
	W(gpu, MMU_INT_CLEAR, 0xffffffff);
	W(gpu, AS0 + AS_TRANSTAB_LO, (uint32_t)(pa_l0 | 0x7));
	W(gpu, AS0 + AS_TRANSTAB_HI, (uint32_t)(pa_l0 >> 32));
	W(gpu, AS0 + AS_MEMATTR_LO, 0x00888d88);
	W(gpu, AS0 + AS_MEMATTR_HI, 0);
	W(gpu, AS0 + AS_TRANSCFG_LO, 0);
	W(gpu, AS0 + AS_TRANSCFG_HI, 0);
	W(gpu, AS0 + AS_COMMAND, AS_CMD_UPDATE);
	if (poll(gpu, AS0 + AS_STATUS, 1, 0, "AS0 update")) return 1;

	/* 5. Job slot 0. */
	W(gpu, JOB_INT_CLEAR, 0xffffffff);
	W(gpu, JS0 + JS_HEAD_NEXT_LO, (uint32_t)GPU_VA);
	W(gpu, JS0 + JS_HEAD_NEXT_HI, (uint32_t)(GPU_VA >> 32));
	W(gpu, JS0 + JS_AFFINITY_NEXT_LO, cores);
	W(gpu, JS0 + JS_AFFINITY_NEXT_HI, 0);
	W(gpu, JS0 + JS_CONFIG_NEXT, JS_CONFIG);
	W(gpu, JS0 + JS_COMMAND_NEXT, 1);        /* START */
	/* Wait for slot 0 done (bit 0) or failed (bit 16). */
	double end = now() + 0.5;
	uint32_t js;
	while (!((js = R(gpu, JOB_INT_RAWSTAT)) & 0x10001) && now() < end)
		;
	printf("JOB_INT_RAWSTAT=%08x JS0_STATUS=%08x MMU_INT_RAWSTAT=%08x AS0_FAULTSTATUS=%08x FAULTADDR=%08x%08x\n",
	       js, R(gpu, JS0 + JS_STATUS), R(gpu, MMU_INT_RAWSTAT), R(gpu, AS0 + AS_FAULTSTATUS),
	       R(gpu, AS0 + AS_FAULTADDR_HI), R(gpu, AS0 + AS_FAULTADDR_LO));

	dcache(data, 4096, 1);
	uint32_t got = data[TARGET_OFFSET / 4];
	printf("job exception_status=0x%08x target=0x%08x -> %s\n", data[0], got, got == MAGIC ? "PASS" : "FAIL");
	return got == MAGIC ? 0 : 2;
}
