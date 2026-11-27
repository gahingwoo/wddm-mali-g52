# History

A few lines per topic. The measurements behind the early milestones are in
[results/](results/README.md); the full investigation logs are in the git
history of `docs/`.

## Milestones

- **Plan, 2026-10-01.** No Windows driver for any Mali existed. DWM
  composes on the display adapter, so a render-only Mali next to Basic
  Display would leave the desktop on WARP: the driver had to own the
  display too.
- **M1, 2026-10-01.** A WRITE_VALUE job ran from Linux userspace with no
  GPU driver (`tools/m1/m1_raw.c`), then in a KMDF driver on Windows
  (`drivers/m1probe`), CM5-IO.
- **M2, 2026-10-02.** Mesa's d3d10umd turned out to be a software-driver
  interface. That allowed Panfrost to drive the Mali under Windows before
  any WDDM code: Mesa 26.2 with Panfrost built for ARM64 in CI.
- **M3 and M4, 2026-10-02.** `malikm` with the Panfrost uAPI; Panfrost
  cleared a render target, then drew a triangle byte-identical to softpipe.
- **M5.1, 2026-10-02.** `malidod`, a display-only driver, drove HDMI. The
  first M5 plan (WDDM 2.0 with GpuMmu at once) was replaced by a WDDM 1.3
  driver after the viogpu3d precedent.
- **M5.2 (b) and (c), 2026-10-02 and 10-03.** A job, then the triangle,
  through the WDDM driver in WinPE, with Mesa loaded by the runtime as the
  UMD. A WinPE shutdown crash on 2026-10-03 was traced to USB, not the GPU.
- **M5.2 (d), 2026-11-13.** On the TaishanPi 3M, the sign-in screen and the
  desktop composed on the Mali, at a frame or two a second.

## Present path and speed

- 2026-11-13/14: the CPU rotate, the primary copy and the vsync-time
  framebuffer copy were removed one at a time. VOP2 scan-out through the
  IOMMU (2026-11-14) took it to 268 presents in 5 s. On by default since
  2026-11-21.
- 2026-11-14: x64 and x86 programs and the OpenGL 3.1 ICD; FurMark ran.
- 2026-11-23, `UmdMergeSubmit`: DWM's submit CPU per job fell from 0.29 to
  0.33 ms to 0.20 to 0.23 ms over three runs. Made the default.
- 2026-11-24, `UmdCopyBatch`: DWM's GPU copy time per 15 s drag fell from
  about 1075 ms to 60 to 123 ms, submits halved, copytest passed. Made the
  default.

## GPU clock

- The GPU ran at the firmware's 198 MHz until 2026-11-14 (FurMark 106),
  then 594 MHz (285 to 290).
- 2026-11-23: the rail read 750 mV, the PMIC default; the firmware raised
  it to 775 mV. 2026-11-24: 702 MHz scored 334 to 336, 49.9 C peak with the
  fan. Since 2026-11-25, 702 MHz is the default where the firmware reports
  775 mV or more, and a fresh install starts at 594 MHz instead of 198.
- 2026-11-24, thermal fallback: the first drop at 85 C caused a TDR because
  the mux and divider were written together while a chain ran. Now one
  field at a time between chains, with a 90 s restore hold and a fault
  policy.
- 2026-11-24/25: four TDRs with no job fault in a 5-minute FurMark run.
  dxgmms2 forces preemption despite `EnablePreemption` 0; fixed by
  answering it at `TimeSlice` 0 (`PreemptAnswer`).
- 2026-11-24: Task Manager shows the GPU temperature at WDDM 2.0. The
  documented "WDDM 2.4" requirement is not checked by the 22621 binaries.

## WDDM 2.0

- 2026-11-13: feasibility study for D3D11 and WDDM 2.x. 2026-11-21: design
  study (IoMmu impossible without an SMMU; GpuMmu, 1.3 with a D3D11.1 UMD,
  D3D12 and 3.x weighed); 2.x approved.
- 2026-11-22: the first 2.0 probe hung after QAI 11/15. The Panorama G615
  and G610 drivers showed a copy paging node and dxgkrnl's adapter handle.
- 2026-11-23: bugcheck 0x139 traced to missing CalibrateGpuClock and
  SetStablePowerState plus a use-after-free in teardown; then 0x113/0x1C
  (rotation Offset0); then a black screen (GetHandleData returns NULL at
  2.x).
- 2026-11-24: renders refused until the UMD made buffers resident; 0x7E in
  dxgmms2 from an empty local budget group. After both, the first 2.0
  desktop on HDMI, and 2.0 became the default the same day.
- 2026-11-24: a power cut before the confirm left the board on 1.3. The
  fallback now confirms in 20 s and judges an unconfirmed boot by how far
  it got.

## Edge and Chrome

- 2026-11-21/22, device creation: Chromium needs ANGLE ES 3.0, which needs
  feature level 10_1 (`UmdDdi` 101) and multisample answers. The first
  answers broke DWM or were refused; per-family answers worked. `UmdDdi`
  101 is the default since 2026-11-25.
- 2026-11-22 to 24, present: Edge lost its device at the first
  composition present. Three UMD bugs were fixed (a draw with no input
  layout, a cached buffer for a shared resource, no log from
  low-integrity processes). The cause was missing DXGI redirection; with
  it, DWM lost its device until a deadlock in the removal logging was
  fixed. Then Edge rendered at 1.3 with DWM alive.
- 2026-11-24/25, at WDDM 2.0: redirection for the browsers froze the whole
  session (a win32k lock, driver idle). `UmdRedirect` 1 became the 2.x
  default and the watchdog `Wd_*` values were added. Edge falls back to
  software at 2.x.
- 2026-11-27: the freeze and the white Edge window explained and fixed
  (EDGE-PRESENT.md): the UMD listed the flip buffers DWM opens as written,
  which dxgkrnl refuses. `UmdRedirect` 3 is the 2.x default.

## Display

- 2026-11-22, HDMI hot-plug: level 1 worked first time (HPD on bit 3,
  256-byte EDID in 296 ms). The 1 s debounce comes from a 0.66 s replug
  bounce seen under Linux.
- 2026-11-22, black desktop at level 3: a primary refused at map time, then
  primaries described at 60/1 on a 59.998 Hz mode. Fixed by describing the
  committed refresh rate exactly. Two connected targets on one source left
  HDMI black; reporting one at a time worked and became `HdmiExclusive`.
- 2026-11-22: a MEMORY_MANAGEMENT bugcheck after a panel restore led to a
  fix for a race on the framebuffer pointer and size.
- 2026-11-23: the TaishanPi 3M's DSI ribbon broke; later runs were on HDMI.
  `HdmiExtend` is written and host-tested but not run.
- 2026-11-24: refresh rates on an AOC Q27B3S2: 60 and 75 Hz at 2560x1440,
  75 Hz kept across a reboot, hot-plug and the 15 s revert worked. Level 3
  became the HDMI boot default. 2026-11-25: largest size at 60 Hz as the
  preferred mode (`HdmiDefault60`).

## Video decode

- 2026-11-24: `vdpu383test` decoded the H.264 conformance streams
  bit-exact against FFmpeg, also from DXVA parameters alone, so the
  decoder went into the UMD rather than a Media Foundation transform.
- 2026-11-25: HEVC Main, VP9 profile 0 and AV1 Main. FFmpeg
  `-hwaccel d3d11va` matched software for H.264, HEVC and VP9; AV1 read
  `frame_refs[i].Index` as a surface index instead of a reference slot,
  fixed off the board.
- 2026-11-25: the copy path cost 15 to 20 ms CPU per 1080p picture against
  2.2 ms in the decoder, which led to zero copy (default on, CI only).
- 2026-11-25, Chromium: the probe showed feature level 10_1 as the only
  blocker; `UmdReportFL11` followed (CI only).
