// SPDX-License-Identifier: GPL-2.0
/*
 * M1, step 1: make the Mali-G52 run one WRITE_VALUE job through Linux's
 * Panfrost driver, so the job layout is known good before anything replaces
 * Panfrost. Prints the job's GPU address so the page table Panfrost built for
 * it can be read back afterwards.
 *
 * Layout from Mesa src/panfrost/genxml/v7.xml (Bifrost):
 *   Job Header (32 bytes)
 *     w0 exception status, w1 first incomplete task, w2-3 fault pointer
 *     w4 bit0 is_64b, bits1-7 type, bit8 barrier, bits16-31 index
 *     w5 dependencies, w6-7 next job
 *   Write Value Job Payload at +32
 *     w0-1 address, w2 type, w4-5 immediate value
 *
 * Build: gcc -O2 -o write_value_panfrost write_value_panfrost.c
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/panfrost_drm.h>

#define JOB_TYPE_WRITE_VALUE   2
#define WRITE_VALUE_IMM32      6
#define TARGET_OFFSET          0x100
#define MAGIC                  0xC0FFEE42u

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int fd = open(node, O_RDWR);
	if (fd < 0) { perror(node); return 1; }

	struct drm_panfrost_create_bo cb = { .size = 4096 };
	if (ioctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &cb)) { perror("CREATE_BO"); return 1; }

	struct drm_panfrost_mmap_bo mb = { .handle = cb.handle };
	if (ioctl(fd, DRM_IOCTL_PANFROST_MMAP_BO, &mb)) { perror("MMAP_BO"); return 1; }
	volatile uint32_t *cpu = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mb.offset);
	if (cpu == MAP_FAILED) { perror("mmap"); return 1; }

	uint64_t va = cb.offset;
	uint64_t target = va + TARGET_OFFSET;
	memset((void *)cpu, 0, 4096);

	cpu[4] = 1u | (JOB_TYPE_WRITE_VALUE << 1) | (1u << 16);   /* is_64b, type, index 1 */
	cpu[8] = (uint32_t)target;                               /* payload: address */
	cpu[9] = (uint32_t)(target >> 32);
	cpu[10] = WRITE_VALUE_IMM32;                             /* payload: type */
	cpu[12] = MAGIC;                                         /* payload: immediate */
	__sync_synchronize();

	uint32_t handles[1] = { cb.handle };
	struct drm_panfrost_submit sub = {
		.jc = va,
		.bo_handles = (uintptr_t)handles,
		.bo_handle_count = 1,
	};
	if (ioctl(fd, DRM_IOCTL_PANFROST_SUBMIT, &sub)) { perror("SUBMIT"); return 1; }

	/* timeout_ns is an absolute CLOCK_MONOTONIC time, not a duration. */
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	struct drm_panfrost_wait_bo wb = {
		.handle = cb.handle,
		.timeout_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec + 2000000000,
	};
	if (ioctl(fd, DRM_IOCTL_PANFROST_WAIT_BO, &wb)) { perror("WAIT_BO"); return 1; }

	uint32_t got = cpu[TARGET_OFFSET / 4];
	printf("gpu_va=0x%llx job exception_status=0x%08x target=0x%08x -> %s\n",
	       (unsigned long long)va, cpu[0], got, got == MAGIC ? "PASS" : "FAIL");
	if (argc > 2 && !strcmp(argv[2], "hold")) {
		/* Keep the BO mapped so its page table can be read back. */
		printf("holding for 20 s\n");
		fflush(stdout);
		sleep(20);
	}
	return got == MAGIC ? 0 : 2;
}
