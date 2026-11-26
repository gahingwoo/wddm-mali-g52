# Vulkan on the Mali-G52 under Windows: panvk on maliwddm

Study and plan, 2026-11-25; merged to `main` 2026-11-26 after a board run
(section 10). Sources are Mesa 26.2.0 (the tag the rest of the UMD is built
from), read in `src/panfrost/vulkan`, `src/vulkan/runtime` and
`src/vulkan/wsi`, the Vulkan-Loader's `docs/LoaderDriverInterface.md`, and
this repository.

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

What it took, once built (section 8): about 280 added lines of Mesa
patches, about 400 in the shim, a CI job, an INF entry and an 850-line test
tool; four CI rounds from the first build to green. The estimate before
starting was: Mesa patches of about 300 to 500 lines
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

One heap, `DEVICE_LOCAL`. On Linux it is sized by `os_get_gpu_heap_size`
from system memory (12280 MiB on the CI runner); the Windows patch caps it
at 1 GB, the size of maliwddm's one aperture segment, which DWM shares.
The memory types are honest for this KMD: no HOST_CACHED type, so CPU reads of
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

- `mesa-patches/vulkan/` (applied only by the Vulkan job in `mesa.yml`, after the shared
  series; see section 10 for why not in `mesa-patches/`):
  - building panvk for Windows: the exports from `vulkan_api.def`,
    `vk_drm_syncobj.c` and the libdrm sync provider compiled into it, the
    POSIX calls the compiler found;
  - Windows enumeration: one physical device on the fd `malikm_open()`
    returns (maliwddm through D3DKMT; `MALIKM_FAKE` in CI), no
    `drmGetDevices2`, the adapter's LUID reported;
  - a build ID from the Mesa version where there is no ELF note;
  - a 32 MB tiler heap and a heap of at most 1 GB on Windows;
  - the device's libdrm sync provider (`vk_device_set_drm_fd` is an inline
    that finds no libdrm in files built without `HAVE_LIBDRM`);
  - `VK_KHR_surface`, `VK_KHR_win32_surface` and `VK_KHR_swapchain`, and the
    WSI's CPU swap chain as a "sw" device.
- `win32-compat/`: `drmGetCap`, `drmGetDevice2`, `drmFreeDevice`,
  `drmSyncobjQuery2`, `drmSyncobjTimelineSignal`, a minimal `drmDevice`,
  `sched.h`; per-syncobj state behind `malikm_syncobj_state()`; the
  adapter's LUID (`malikm_adapter_luid()`). Default behaviour unchanged.
- `mesa.yml`, job "Vulkan ICD, panvk (ARM64)": the ARM64 build (the GL ICD's
  configuration plus `-Dvulkan-drivers=panfrost`), the manifest, and the
  test tool against the emulated driver.
- `tools/windows/vkprobe.cpp`: loads the ICD directly, prints what
  `vulkaninfo` would (version, limits, memory types, queues, formats,
  extensions), then creates a device, a buffer, a fill and a copy, a render
  pass clearing an image, and with `--window` a Win32 surface and swap chain
  that presents a few cleared frames.
- `drivers/maliwddm/maliwddm.inf`: `VulkanDriverName` pointing at
  `%13%\malivk.json` next to `malivk_arm64.dll`. Only on this branch.

### What CI shows (emulated kernel driver, 2026-11-25)

`mesa.yml`, job "Vulkan ICD, panvk (ARM64)", run 45549976958 on `8a4c5fa`,
green. `malivk_arm64.dll` is an ARM64 DLL that imports only KERNEL32,
USER32, GDI32 and an api-ms-win-core set, and exports the three
`vk_icd*` entry points. vkprobe against it:

- No Mali and no emulation: the instance is created and reports 0 physical
  devices (exit 2), with the shim's "cannot open \\.\MaliG52, and no
  maliwddm adapter".
- With `MALIKM_FAKE=1`: one device, "Mali-G52 r1 MC3", apiVersion 1.0.354,
  driver "panvk, Mesa 26.2.0", conformance 0.0.0.0; 130 device extensions;
  `geometryShader` 0, `tessellationShader` 0, BC/ETC2/ASTC 0 (the emulated
  GPU reports none; the board's answer may differ); two memory types as
  in section 5; one queue family (graphics, compute, transfer), no
  timestamps.
- Every fence and event answer right: a new fence is not ready and a wait
  of 0 or 20 ms times out, a signalled one is ready and not after a reset,
  an event reads reset, set, reset; a submitted fence signals.
- 61 job chains submitted through the shim for the fill, copy, clear and
  the presents. Their results are not judged: nothing executes them.
- The window: a Win32 surface (3 formats), a 640x480 swap chain of two
  B8G8R8A8 images, 8791 frames presented in 3 s through the CPU path.
  That rate means nothing for the board (no GPU work, no wait); it shows
  the acquire, submit, fence and blit loop does not stall or leak.

What CI cannot show: that the jobs panvk builds run correctly on the G52,
that the shim's waits on maliwddm (D3DKMTWaitForIdle, the statistics
escape) answer as the emulated ones do, and the speed of the readback.

## 9. Board test plan

TaishanPi 3M on HDMI, Windows 11 23H2. Use the `wddm-mali-g52-driver-arm64`
artifact of this branch's CI run (the `vulkan-panvk` PR lists it). It is the
normal maliwddm package plus `malivk_arm64.dll`, `malivk.json` and
`vkprobe.exe`.

1. Install the package the usual way (INSTALL.txt in the artifact), then
   reboot. Check the registry entry and the file it names:

   ```
   reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" /s /v VulkanDriverName
   dir C:\Windows\System32\DriverStore\FileRepository\maliwddm.inf_arm64_*\malivk*
   ```

2. Run `vkprobe.exe` from the artifact folder. With no argument it loads
   `malivk_arm64.dll` from beside itself and sets
   `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1` for itself. Expect "Mali-G52",
   apiVersion 1.0, two memory types and one queue family. The fill, copy
   and 256x256 clear must read back as written. This is the first GPU
   work through panvk. A wrong value or a TDR here is a kernel-interface
   problem, not a WSI one.
3. `vkprobe.exe --window 10`: a window cycling colours for ten seconds,
   then the frame rate.
4. Through the real loader. Windows does not ship `vulkan-1.dll`, so
   install the LunarG Vulkan SDK for ARM64 first
   (`vulkansdk-windows-ARM64-<version>.exe`, about 537 MB; LunarG has no
   runtime-only installer for ARM64). Then set
   the variable for every program and sign out and back in:

   ```
   setx /M PAN_I_WANT_A_BROKEN_VULKAN_DRIVER 1
   vkprobe.exe loader
   vulkaninfo --summary
   vkcube
   ```

   `vkprobe.exe loader` must find the same device as step 2. If it finds
   none, the loader did not take `VulkanDriverName`. In that case, set
   `VK_DRIVER_FILES` to the `malivk.json` path from step 1 and run it again
   to tell the registry apart from the ICD.
5. On failure, collect the vkprobe output, a run with `MALIKM_TRACE=1`,
   `C:\mali\` logs and System events 4101 (TDR).
6. Regression: the desktop, FurMark (OpenGL) and a D3D11 program behave as
   on `main`.

Back out: install a package from `main`, which has no `VulkanDriverName`,
and `reg delete "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v PAN_I_WANT_A_BROKEN_VULKAN_DRIVER /f`.

## 10. Board results, 2026-11-26

Windows 11 23H2, package from CI run 45785802923 (maliwddm 8.45.21.568).

| Check | TaishanPi 3M | ROCK 4D (12 GB) |
|---|---|---|
| `vkprobe.exe`: device, sync, fill+copy, 256x256 clear | all correct | not run |
| `vkprobe.exe --window 10` | 63 frames in 10.09 s (6.2/s) | not run |
| LunarG SDK 1.4.363.1 loader, `vulkaninfo --summary` | not run (no disk space) | GPU0 panvk, apiVersion 1.0.354 |
| `vkcube --c 300` | not run | 300 frames in 50.1 s (about 6/s), exit 0 |
| TDR (event 4101), dwm crashes, D3D (`d3dprobe`) | none, D3D unaffected | none |

The loader finds the ICD through `VulkanDriverName` with no
`VK_DRIVER_FILES`. Programs still need `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`
(`setx /M`, then sign out and in). The frame rate is the WSI's CPU path
(section 4), not the GPU.
