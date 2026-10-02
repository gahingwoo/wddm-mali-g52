# M5.2 step (c), first half: Panfrost on the WDDM driver, 2026-10-02

**Passed** in WinPE (22621) on the CM5-IO, run 16 ([output](pe-run16.txt)).

M4's D3D11 triangle (`tests/d3d11-triangle`), rendered by the Mali with the
whole stack on WDDM:

    D3D11 runtime -> Mesa d3d10umd (software DDI) -> Panfrost
      -> win32-compat libdrm shim, maliwddm backend (D3DKMT system calls)
      -> maliwddm (WDDM 1.3 KMD) -> Mali-G52

![triangle](triangle-on-mali-through-wddm.png)

The image is **byte-identical** to M4's, which went through malikm and was
itself byte-identical to softpipe.

maliwddm's counters after the triangle: 9 submissions, 9 jobs done, 0 failed,
0 faults, 0 resets, 9 interrupts; VidMm mapped 14 allocations into the
aperture and unmapped 14 (paging Panfrost's BOs in and out around the
submissions, at their fixed GPU addresses); every patched DMA buffer carried
its BOs (last: 9 allocations).

## How

- The shim (`win32-compat/malikm_drm.c`) falls back to maliwddm when
  `\\.\MaliG52` cannot be opened. Panfrost's uAPI maps onto D3DKMT:
  CREATE_BO -> CreateAllocation + `MW_ESCAPE_ALLOC_INFO`; mmap -> a Lock
  (IgnoreSync) held for the BO's life; SUBMIT -> Render of one `MW_CMD_JOB`
  with the BOs as the allocation list; waits -> WaitForIdle. Thunks come from
  win32u (`NtGdiDdDDI*`).
- WinPE has no D3D11 runtime: d3d11, dxgi, D3DCompiler_47, DXCore and the VC
  runtime come from the installed Windows (22621) on the stick and are copied
  into the RAM disk; the import closure was computed against WinPE's file list.

## Not yet

- This is d3d10umd's *software* DDI loading Panfrost, with the shim talking
  to the KMD directly. Step (c) proper is a hardware UMD: d3d10umd's WDDM
  path through the runtime callbacks (pfnAllocateCb, pfnRenderCb, ...), from
  Mesa MR 24223's gdikmt winsys, with Panfrost's kmod behind it.
- Present (step d), DWM (step e).
