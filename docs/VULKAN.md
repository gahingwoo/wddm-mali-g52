# Vulkan on the Mali-G52 under Windows: panvk on maliwddm

Study and plan, 2026-11-25, with the first implementation on branch
`vulkan-panvk`. Nothing here has run on a board yet. Sources are Mesa
26.2.0 (the tag the rest of the UMD is built from), read in
`src/panfrost/vulkan`, `src/vulkan/runtime` and `src/vulkan/wsi`, the
Vulkan-Loader's `docs/LoaderDriverInterface.md`, and this repository.

## 1. The verdict

A minimal Vulkan driver is feasible and cheap, because almost everything it
needs below Mesa already exists: panvk talks to the kernel through the same
`pan_kmod` panfrost backend as the Gallium driver, and our shim
(`win32-compat/malikm_drm.c`) already turns that backend's DRM calls into
maliwddm calls through D3DKMT, the way the OpenGL ICD uses it. No new kernel
interface and no KMD change is needed for a first triangle.

What it will be, and what it will not:

- **Vulkan 1.0, non-conformant, behind an opt-in.** That is what Mesa's
  panvk offers the G52 (Bifrost v7) on Linux too: `get_api_version()`
  returns 1.4 only for `PAN_ARCH >= 10` and 1.0 otherwise
  (`panvk_vX_physical_device.c:767`), the conformance version is 0.0.0.0
  for everything but v10, and `panvk_physical_device_init` refuses v6, v7
  and v14 unless `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER` is set
  (`panvk_physical_device.c:399`). `docs/drivers/panfrost.rst` lists
  "G31, G52, G76, Bifrost (v7), Vulkan 1.0". `docs/features.txt` has panvk
  under Vulkan 1.0 and only `panvk/v10+` under 1.1.
- **Enough for vulkaninfo, vkcube-class programs and simple compute.** A
  window gets its frames by the WSI's CPU path (section 4): a GPU copy into a
  host-visible buffer, then GDI. The same cost the OpenGL ICD pays today.
- **Not enough for DXVK, VKD3D-Proton or modern Vulkan games** (section 7).
  Minecraft's Vulkan renderer and the like want 1.2 or newer.

Estimated size of the minimal path: Mesa patches of about 300 to 500 lines
(Windows enumeration, build-id, a few POSIX calls, the build files), about
250 lines in the shim (the libdrm calls panvk adds, and syncobjs that keep
their own state), a CI job, an INF change and a test tool. The unknown is
how many POSIX-isms the Windows compiler finds in panvk's 60-odd files; CI
lists them all in one run (`ninja -k 0`). The work is days, not weeks;
the board test is where the real risk is (section 9).

## 2. panvk on Bifrost v7 in Mesa 26.2

What v7 gets, read from `panvk_vX_physical_device.c` (features with
`PAN_ARCH >= 10` or `>= 9` are off on the G52):

| | v7 (G52) | v10 (G610) |
|---|---|---|
| API version | 1.0 | 1.4 |
| Conformance | none | 1.4.1.2 |
| Gate | `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` | none |
| Queue | one GPU queue (JM, `jm/panvk_vX_gpu_queue.c`) | two (CSF) |
| `geometryShader`, `tessellationShader` | no | no |
| transform feedback | no | no |
| `shaderFloat16`, `multiDrawIndirect`, `drawIndirectCount` | no | yes |
| `robustness2`, `nullDescriptor` | no | yes |
| descriptor indexing | no | from v9 |
| `samplerFilterMinmax`, `conditionalRendering` | no | yes |
| timeline semaphores | yes, emulated on binary syncobjs (`vk_sync_timeline`) | native |
| dynamic rendering, synchronization2 | yes (extensions) | yes |
| BC textures | if the GPU has them (`has_texture_compression_bc`) | same |

So the extensions that make up Vulkan 1.1 and 1.2 are mostly there as
extensions on v7, but the version stays 1.0 because some 1.1/1.2 core
features are missing (`has_vk1_1 = PAN_ARCH >= 10`). Programs that only check
`apiVersion` will refuse it; `MESA_VK_VERSION_OVERRIDE` can lie about it for
experiments, as on Linux.

The memory model panvk uses on v7: one VM per device with kernel-assigned
addresses (`PAN_KMOD_VM_FLAG_AUTO_VA` for `PAN_ARCH < 10`,
`panvk_vX_device.c:416`). That is exactly what maliwddm gives: a fixed GPU
VA per allocation, assigned at creation. No VM_BIND is needed.

## 3. How a Vulkan ICD is found for a WDDM adapter

From the loader's `LoaderDriverInterface.md`, "Driver Discovery on Windows":

- The loader enumerates DXGI adapters and, for each, reads values from the
  adapter's registry key (the one an INF writes with `HKR`):
  `VulkanDriverName` for 64-bit processes and `VulkanDriverNameWow` for
  32-bit ones on 64-bit Windows. `REG_SZ` or `REG_MULTI_SZ`.
- Each value is "a full absolute path to a JSON manifest file". In an INF:
  `HKR,,VulkanDriverName,%REG_MULTI_SZ%,%13%\malivk.json`; `%13%` is the
  driver store directory the package lands in, so the manifest and the DLL
  never have to be copied to System32.
- In the manifest, a relative `library_path` "is relative to the path of the
  JSON manifest file". `library_arch` ("32" or "64") lets a loader skip a
  driver of the wrong width without loading it.
- `vk_icdEnumerateAdapterPhysicalDevices(instance, LUID, ...)` (required
  from loader interface version 7, Windows only) is how the loader matches
  physical devices to adapters for ordering. Mesa's runtime implements it
  (`vk_instance.c:504`) by comparing `deviceLUID`, so panvk has to report the
  maliwddm adapter's LUID with `deviceLUIDValid`. The shim has the adapter
  handle; `D3DKMTEnumAdapters2` gives its LUID.

The document names no ARM64-specific value. An x64 program on ARM64 runs an
x64 loader, which reads `VulkanDriverName` too and would find an ARM64 DLL it
cannot load. The answer is the one the D3D UMD and the OpenGL ICD already use:
an ARM64X forwarder (`maliumd.dll`, `maligl.dll`), with an x64 build of
panvk behind it. That is not in the minimal path; the first package ships
ARM64 only, with `library_arch` "64" and a note that x64 and x86 programs see
no Vulkan device yet.

For CI and for trying a build by hand, `VK_DRIVER_FILES` (or
`VK_ICD_FILENAMES`) pointing at the manifest bypasses the registry. The test
tool in this branch goes further and loads the ICD itself through
`vk_icdGetInstanceProcAddr`, so it needs no `vulkan-1.dll` (GitHub's
windows-11-arm image has no Vulkan runtime).

## 4. WSI: getting a frame into a window

Mesa's Windows WSI (`wsi_common_win32.cpp`) has two swap chains:

- **DXGI**: images are D3D12 resources of a DXGI composition swap chain.
  Chosen only when the driver supplies `win32.get_d3d12_command_queue`
  (`supports_dxgi`, line 963). Only dozen does. Not for us: there is no
  D3D12 on this adapter.
- **CPU ("sw")**: each swap chain image is rendered as usual, then the WSI
  records a copy to a host-visible linear buffer (`WSI_SWAPCHAIN_BUFFER_BLIT`,
  `wsi_configure_cpu_image`, chosen because `wants_linear` is false), maps
  it, copies the rows into a DIB section and `StretchBlt`s it into the
  window's DC (`wsi_win32_queue_present`, line 810). DWM composes the window
  like any GDI window.

The CPU path works for any driver with a host-visible memory type and a
graphics or compute queue, so panvk gets `VK_KHR_win32_surface` and
`VK_KHR_swapchain` for free. Its cost is what the OpenGL ICD pays today
(`0060`, `wgl_panfrost_present`): one GPU copy out of the tiled/AFBC image,
one CPU read of the frame from uncached memory, one GDI blit. At 1024x640
that is fine; at 2560x1440 it is the same wall the GL ICD hits.

panvk passes `sw_device = false` on Linux. On Windows the patch passes
`true`, and it has to: only a "sw" WSI device waits for the copy's fence
before the present reads the buffer (`wsi_common.c:2882`); without it the
CPU would copy a frame the GPU has not finished writing. The same flag keeps
the WSI from asking for `vkGetMemoryFdKHR` and skips the DXGI factory and
DirectComposition device the CPU path never uses.

A faster present later: render into a shared resource the D3D UMD can open
and present (the path DWM's own buffers take, `0034`, `0035`), which is
what Mesa's DXGI swap chain does with D3D12. That needs a Vulkan
external-memory type backed by a runtime resource, and it is not small.

## 5. Memory types

maliwddm has one aperture segment (1 GB, `MW_SEGMENT_SIZE`), every
allocation `CpuVisible`, `Cached` not set, `CacheCoherent` FALSE
(`memory.c`). VidMm therefore maps a `Lock` write-combined, and the shim
keeps that lock for the BO's life (`IOCTL_MALIKM_MAP`). The Mali on the
RK3576 is not IO-coherent.

panvk derives its memory types from the kmod properties
(`get_device_heaps`): `is_io_coherent` and `PAN_KMOD_BO_FLAG_WB_MMAP` need
panfrost uAPI 1.6, and the shim says 1.1. So panvk exposes exactly:

| Type | Flags | Backing |
|---|---|---|
| 0 | DEVICE_LOCAL | an allocation never mapped (imported buffers are meant to land here) |
| 1 | DEVICE_LOCAL, HOST_VISIBLE, HOST_COHERENT | an allocation, write-combined CPU mapping |

One heap, `DEVICE_LOCAL`, sized by `os_get_gpu_heap_size` from system
memory. That is honest for this KMD: no HOST_CACHED type, so CPU reads of
GPU results (the WSI's readback included) are uncached reads. A cached type
would need cached CPU mappings plus cache maintenance the KMD does not
offer.

Two limits panvk does not know about:

- **1 GB resident for the whole machine** (the segment's commit limit), and
  every BO of the device is listed in every submission (the shim's residency
  rule, `wsubmit`). A Vulkan program that allocates more than what is left
  next to DWM gets allocation failures, not paging.
- **256 allocations per submission** (`walloc_size`): the shim lists the
  BOs a job names first, then as many other live BOs as fit. panvk uses
  fewer, larger BOs than a D3D program (pools), so this matters less.

panvk's tiler heap on v7 is 128 MB grow-on-fault on Linux
(`panvk_vX_device.c:493`). maliwddm has no grow-on-fault, so the shim
creates it whole; the Windows patch makes it 32 MB, the size the D3D and GL
paths settled on (`0053`).

## 6. Synchronisation

On Linux panvk's JM queue uses DRM syncobjs: one `queue->sync` that every
submission signals, semaphores and fences as `vk_drm_syncobj`, VkEvent as a
syncobj, and the binary-to-binary transfer by export/import of a sync file
(`panvk_queue_transfer_sync`). Timeline semaphores are emulated on top
(`vk_sync_timeline`), because panfrost syncobjs have no timeline.

What the shim does today on the WDDM path (`wddm_drm`): syncobjs are
numbers with no state. Create returns a handle, signal and reset do nothing,
and a wait waits for the device's whole queue to go idle
(`D3DKMTWaitForIdle` in a process with no runtime device). That is right for
Gallium Panfrost, which only ever waits on fences it has submitted, and the
KMD runs one in-order queue, so GPU-side waits on earlier work always hold.

For Vulkan it is wrong in ways a program sees: `vkGetFenceStatus` and
`vkGetEventStatus` on something never submitted or reset say "signalled",
and a host-signalled event or a reset fence reads the same as a done one.
So the shim gets per-syncobj state, used only when the Vulkan driver asks
for it (`malikm_syncobj_state(1)`), so that the D3D UMD and the GL ICD keep
today's behaviour bit for bit:

- each syncobj is unsignalled, signalled, or pending on a submission number
  (the shim's `wseq`);
- `SUBMIT` with an `out_sync` makes it pending on that submission; signal
  and reset set the state; the sync-file export/import carries the
  submission number across (as it does today for Gallium);
- a wait is done when every handle (or any, without `WAIT_ALL`) is
  signalled or pending on a submission known complete. A submission is
  known complete after a `WaitForIdle`, or when the KMD's statistics escape
  says nothing is queued or running. A zero timeout never blocks; a nonzero
  one waits for idle, as today. An unsignalled syncobj with no submission
  behind it returns `ETIME` at once, and with `WAIT_FOR_SUBMIT` and a
  timeout it is polled until something submits it or the time runs out.

At WDDM 2.0 the KMD could give each submission a monitored fence that a
process waits on with `D3DKMTWaitForSynchronizationObjectFromCpu`, which
would make waits per submission instead of per device. Not needed for the
minimal path, and it is the same work the D3D UMD's waits want
(WDDM2.md 5.3), so it should be done once for both.

## 7. DXVK and the rest

DXVK 2.x requires a Vulkan 1.3 device and, for any D3D11 feature level of
10_0 and up, `geometryShader` and transform feedback (stream output); 11_0
adds tessellation. panvk sets `geometryShader = false` and
`tessellationShader = false` on every architecture, has no
`EXT_transform_feedback`, and on v7 is a 1.0 device without `robustness2` or
`nullDescriptor`, which DXVK also requires. So DXVK will not create a device
on the G52, at any feature level, and nothing short of geometry and
tessellation shaders in panvk (Mali has no hardware stages for either; they
would be emulated in compute, as Asahi's Honeykrisp does, months of work)
changes that. VKD3D-Proton needs far more. Zink (OpenGL on Vulkan) would
work in principle but gives nothing over the GL ICD we already run on
Panfrost directly.

Said plainly: Vulkan on this GPU is for Vulkan programs that ask for little
(samples, simple engines, compute, `vulkaninfo`), not a way to D3D11 FL 11_0
or to D3D12 games.

## 8. What was built on `vulkan-panvk`

- `mesa-patches/vulkan/` (applied only by `vulkan.yml`, after the shared
  series; see section 10 for why not in `mesa-patches/`):
  - building panvk for Windows: the exports from `vulkan_api.def`,
    `vk_drm_syncobj.c` and the libdrm sync provider compiled into it, the
    POSIX calls the compiler found;
  - Windows enumeration: one physical device on the fd `malikm_open()`
    returns (maliwddm through D3DKMT; `MALIKM_FAKE` in CI), no
    `drmGetDevices2`, the adapter's LUID reported;
  - a build ID from the Mesa version where there is no ELF note;
  - a 32 MB tiler heap on Windows;
  - the WSI's CPU swap chain without DXGI.
- `win32-compat/`: `drmGetCap`, `drmGetDevice2`, `drmFreeDevice`,
  `drmSyncobjQuery2`, `drmSyncobjTimelineSignal`, a minimal `drmDevice`,
  `sched.h`; per-syncobj state behind `malikm_syncobj_state()`; the
  adapter's LUID (`malikm_adapter_luid()`). Default behaviour unchanged.
- `.github/workflows/vulkan.yml`: the ARM64 build (the GL ICD's
  configuration plus `-Dvulkan-drivers=panfrost`), the manifest, and the
  test tool against the emulated driver.
- `tools/windows/vkprobe.cpp`: loads the ICD directly, prints what
  `vulkaninfo` would (version, limits, memory types, queues, formats,
  extensions), then creates a device, a buffer, a fill and a copy, a render
  pass clearing an image, and with `--window` a Win32 surface and swap chain
  that presents a few cleared frames.
- `drivers/maliwddm/maliwddm.inf`: `VulkanDriverName` pointing at
  `%13%\malivk.json` next to `malivk_arm64.dll`. Only on this branch.

## 9. Board test plan

On the TaishanPi 3M (HDMI, the DSI ribbon is broken), Windows 11 23H2, the
driver package from this branch's CI:

1. Install the package as usual. Check that
   `HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-...}\000N\VulkanDriverName`
   names a file that exists in the driver store.
2. Set `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` system-wide (setx /M) and log
   off and on. Without it panvk refuses the G52 on purpose.
3. `vkprobe.exe` (no arguments): expect "Mali-G52 r1 MC3" (or as the model
   table names it), apiVersion 1.0, two memory types, one queue family, and
   "fill+copy: ok" with the buffer read back as written. This is the first
   GPU work through panvk; a wrong value or a TDR here is a kernel-interface
   problem, not a WSI one.
4. `vkprobe.exe --clear`: a render pass clearing a 256x256 image, copied to
   a buffer and checked on the CPU.
5. `vkprobe.exe --window`: a window cycling through colours for ten seconds;
   the frame rate is printed.
6. If a Vulkan SDK is at hand: `vulkaninfo --summary` and `vkcube`
   (ARM64 builds). They go through the real loader and so test the
   registry entry.
7. Collect: `C:\mali\` logs, `MALIKM_TRACE=1` output of a failing run,
   Event Viewer for TDRs (event 4101).
8. Regression: the desktop, an OpenGL program (FurMark) and a D3D11 one
   must behave as before. The shim's default paths did not change, but this
   checks it.

Back-out: install a package from `main`; it has no `VulkanDriverName`.

## 10. Why this is not on `main`

It needs a board run (section 9) before anyone relies on it, and two parts
of it are shared: the shim (one source file, linked into the D3D UMD, the
GL ICD and now the Vulkan ICD) and the INF. The shim's new behaviour is off
unless the Vulkan driver asks for it, and the new libdrm functions are only
called by panvk, so the D3D and GL paths should be bit-for-bit what they
were; CI's existing jobs are the check. The Mesa patches live in their own
directory, applied only by the Vulkan job, so they cannot collide with the
numbers the OpenCL and video branches take in `mesa-patches/`, and the D3D
and GL builds never see them.

If the board run is good, merging is: the shim, `mesa-patches/vulkan/`,
`vulkan.yml`, the tool, and the INF and package changes, with the ICD
installed by default only after a second board passes.
