//
// cache.asm - data cache maintenance by virtual address, for the cached
// allocations a windowed present is copied from (MW_ALLOC_CACHED)
//
// The Mali is not coherent with the CPU caches. An allocation VidMm backs
// with cached pages is read by the CPU at memory speed (a write-combined
// one at about 100 MB/s), but the CPU's caches then hold lines the GPU has
// since written over, so before the CPU reads what the GPU wrote the range
// is cleaned and invalidated (DC CIVAC). The WDK has no intrinsic for DC,
// hence these leaf functions (no stack, no unwind data needed), the same
// as woa-rk3576's rknpu driver has. Maintenance by VA to the point of
// coherency is broadcast to every CPU in the inner shareable domain, so it
// does not matter which CPU runs it or which mapping of the page is used.
//
//   VOID MwDcClean(PVOID Va, SIZE_T Length)            dc cvac,  then dsb sy
//   VOID MwDcCleanInvalidate(PVOID Va, SIZE_T Length)  dc civac, then dsb sy
//
// The line size is CTR_EL0.DminLine (64 bytes on Cortex-A72 and A53).
//
// Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
// SPDX-License-Identifier: GPL-2.0-only
//

	EXPORT MwDcClean
	EXPORT MwDcCleanInvalidate
	AREA   s_MwCache, CODE, READONLY

MwDcClean
	cbz   x1, MwDcClean_done
	mrs   x3, CTR_EL0
	ubfx  x3, x3, #16, #4
	mov   x2, #4
	lsl   x2, x2, x3
	add   x1, x0, x1
	sub   x3, x2, #1
	bic   x0, x0, x3
MwDcClean_loop
	dc    cvac, x0
	add   x0, x0, x2
	cmp   x0, x1
	blo   MwDcClean_loop
MwDcClean_done
	dsb   sy
	ret

MwDcCleanInvalidate
	cbz   x1, MwDcCleanInvalidate_done
	mrs   x3, CTR_EL0
	ubfx  x3, x3, #16, #4
	mov   x2, #4
	lsl   x2, x2, x3
	add   x1, x0, x1
	sub   x3, x2, #1
	bic   x0, x0, x3
MwDcCleanInvalidate_loop
	dc    civac, x0
	add   x0, x0, x2
	cmp   x0, x1
	blo   MwDcCleanInvalidate_loop
MwDcCleanInvalidate_done
	dsb   sy
	ret

	END
