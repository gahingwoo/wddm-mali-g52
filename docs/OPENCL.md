# OpenCL on the Mali-G52: rusticl on Panfrost

Written 2026-11-25, on branch `opencl-rusticl`; on `main` since 2026-11-26.
Board run on the TaishanPi 3M, 2026-11-26: vecadd over 16M floats at
8.1 GB/s, tiled SGEMM 1024x1024 at 8.26 GFLOP/s, results correct. The question was whether Mesa's OpenCL frontend,
rusticl, can give Windows 11 23H2 ARM64 programs OpenCL on the RK3576's
Mali-G52, on the same Panfrost and kernel driver that already run Direct3D
and OpenGL.

**Verdict: yes, and it is built.** rusticl compiles for Windows ARM64 with
two small patches, and the LLVM it needs is the LLVM project's own Windows
ARM64 release, so CI never builds LLVM. Panfrost is a rusticl driver Mesa
enables by default for Bifrost. The kernel driver needs no change. What is
missing is a board run. The costs are a large DLL (clang and LLVM are
linked in), no x64 or x86 ICD yet, and Windows' 2 s GPU timeout for long
kernels. CI results are in [CI](#ci), below.

## The pieces

```
application (ARM64)
  -> OpenCL.dll            Khronos ICD loader, System32 (ours, if none was there)
  -> malicl_arm64.dll      rusticl + Panfrost, in the driver store,
                           named by the adapter's OpenCLDriverName
       OpenCL C -> clang -> LLVM IR -> SPIR-V (SPIRV-LLVM-Translator)
                -> NIR (libclc linked in) -> Bifrost code (Panfrost's compiler)
       buffers, compute jobs -> Panfrost -> libpankmod's libdrm shim
  -> D3DKMT (gdi32)        CreateAllocation, Render, Lock: the same path the
                           OpenGL ICD takes (mesa-patches/0060)
  -> maliwddm.sys          unchanged: a compute job chain goes to job slot 1
```

## 1. Does rusticl build for Windows?

Mesa does not build it there. Mesa's own Windows CI builds LLVM, clang,
SPIRV-LLVM-Translator, libclc and Rust, and still sets
`-Dgallium-rusticl=false` (`.gitlab-ci/windows/mesa_build.ps1` in 26.2.0);
its Windows OpenCL is `microsoft-clc` for OpenCLOn12. Nothing in Mesa says
rusticl is meant to work on Windows, and no merge request was found that
tries. So this is a port, and the work it took is the list of Linux-isms
in rusticl, which is short:

| What | Where | Done here |
|---|---|---|
| `dlfcn.h`, `unistd.h` | `rusticl_libc_bindings.h` (bindgen input) | 0102: `io.h` on Windows, a `dlsym` that finds nothing (it only looks up `eglGetProcAddress`/`glXGetProcAddress` for GL sharing, which Windows has neither of) |
| `sys/mman.h`, `mmap` | `rusticl_mesa_bindings.h`, `core/context.rs` | nothing: win32-compat has the header and libpankmod the function; the call is in the SVM path, which is `cfg(linux/freebsd)` |
| `close()` on a fence fd | `mesa/pipe/fence.rs` | the CRT's POSIX name; only for external semaphores |
| ICD exports | `targets/rusticl/rusticl.sym` is a GNU version script | 0102: `rusticl.def` with the same three names |
| No `pipe_loader_drm` (no libdrm) | `pipe_loader_probe` finds only the software device | 0101: a pipe-loader backend for the Mali (below) |
| The shim's log symbols | `DebugPrintf`, `mali_slow_*`, defined by the D3D frontend | 0102: in `rusticl_target.c`, as the GL target has them |

Rust itself: `aarch64-pc-windows-msvc` is a tier 2 target with host
tools, rustup installs it on the `windows-11-arm` runner, and bindgen
(0.71.1, built with cargo) runs against the release's `libclang.dll`.
Meson 1.7 or later builds Mesa's Rust on MSVC. Anything left (bindgen's
MSVC enum types, for instance) shows up as compile errors in CI; the CI
section below says what it found.

### LLVM, clang, the translator and libclc on Windows ARM64

rusticl compiles OpenCL C at run time (`clBuildProgram`), so the compiler
ships inside the ICD. There is no "offline kernels" way around that: an
OpenCL program hands the driver source or SPIR-V when it runs. (Only
rusticl's own few built-in kernels are compiled at build time.)

| Dependency | Source | Size |
|---|---|---|
| LLVM + clang 20.1.8, static libraries | `clang+llvm-20.1.8-aarch64-pc-windows-msvc.tar.xz` from the LLVM project's GitHub release: built with `LLVM_INSTALL_TOOLCHAIN_ONLY=OFF` (`llvm/utils/release/build_llvm_release.bat`), so it has `lib/*.lib`, `lib/cmake/llvm`, `llvm-config.exe` and clang's headers | 900 MB download; pruned and cached by `actions/cache` |
| SPIRV-LLVM-Translator v20.1.9 | built in CI against that LLVM (a few minutes), cached | a static library |
| SPIRV-Tools, SPIRV-Headers vulkan-sdk-1.4.321.0 | built in CI, cached | |
| libclc | Ubuntu's `libclc-20` `.spv` files (SPIR-V, platform independent) from the Linux job, embedded with `-Dstatic-libclc=all` | 2.5 MB each |
| pkgconf 2.3.0 | built in CI (meson needs pkg-config for the three above) | |

The LLVM release exists for Windows ARM64 from 20.1 on (19.1.7 has only
the `woa64.exe` installer, no tarball); 21, 22 and 23 have it too. 20 was
chosen to match Ubuntu's libclc-20; Mesa 26.2 accepts 15 and up.

The release's static libraries use the static C runtime: `dumpbin
/directives LLVMSupport.lib` gives `/DEFAULTLIB:libcmt.lib` (CI run
45546040765). That is what the D3D UMD and the GL ICD use (`/MT`,
`-Db_vscrt=mt`), so this build is `/MT` too, Rust's standard library with
it (`-Ctarget-feature=+crt-static`; the `windows-msvc` default is the DLL
runtime), and SPIRV-Tools and the translator are built with
`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`. The ICD then needs no VC
runtime DLL: CI fails the build if it imports anything but Windows' own
DLLs. (The release builds libxml2 for `LLVMWindowsManifest`; Mesa does
not link that library, so libxml2 is not needed.)

## 2. Does rusticl support Panfrost, and how well on Bifrost?

- `rusticl: enable panfrost by default` landed 2026-07-09 and is in 26.2.0:
  `mesa/pipe/device.rs` puts `panfrost` in the default `RUSTICL_ENABLE`
  list for `gallium-rusticl-enable-drivers=auto`. So no environment
  variable is needed.
- Mesa's Panfrost page on main lists OpenCL 3.0 for every Panfrost
  generation, G52 (Bifrost v7) included. The Khronos conformance
  submission it cites is for **Mali-G310 and Mali-G610** (Valhall v10),
  not for the G52. On Bifrost OpenCL 3.0 is implemented, not
  conformance-tested by a submission. (26.2.0's own copy of the page
  still says "OpenCL is not supported at this time"; main's is newer.)
- Bifrost has no FP64, so `cl_khr_fp64` is absent; OpenCL 3.0 makes it
  optional. Images, local memory, barriers and atomics are there through
  Panfrost's compute support (the same compute jobs GLES 3.1 uses).
- rusticl itself: OpenCL 3.0 "all DONE" in `docs/features.txt`.

## 3. How the ICD is found on Windows

The Khronos loader (OpenCL-ICD-Loader, `loader/windows/icd_windows.c`,
`khrIcdOsVendorsEnumerate`) looks in four places, in order:

1. **DXGK** (`icd_windows_dxgk.c`): `D3DKMTEnumAdapters3/2`, then for each
   adapter `D3DKMTQueryAdapterInfo(KMTQAITYPE_QUERYREGISTRY)` for the
   value `OpenCLDriverName` (`OpenCLDriverNameWow` in a 32-bit process on
   64-bit Windows) of the adapter's key, `REG_SZ` or `REG_MULTI_SZ`, with
   `translate_path` set. dxgkrnl answers this itself from the registry;
   the KMD is not asked.
2. **HKR** (`icd_windows_hkr.c`): every present, started display-class
   device node; `CM_Open_DevNode_Key(CM_REGISTRY_SOFTWARE)` and the same
   value; the string is passed to `LoadLibraryExA` as it is, so it must
   be a full path.
3. App packages (OpenCLOn12's way).
4. `HKLM\SOFTWARE\Khronos\OpenCL\Vendors` (a value per ICD path).

The loader keeps one entry per loaded module (`khrIcdVendorAdd` compares
library handles), so when 1 and 2 find the same file there is one
platform, not two. `maliwddm.inf` writes
`HKR,,OpenCLDriverName,%REG_SZ%,%13%\malicl_arm64.dll`: a full
driver-store path, which works for 2 and is what translate_path
expects for 1. Nothing is written under `HKLM\SOFTWARE\Khronos`.

Windows 11 has no `OpenCL.dll` of its own (the Store's "OpenCL, OpenGL
and Vulkan Compatibility Pack" brings one inside its app package), and
programs link to `OpenCL.dll` by name, so the package installs the
Khronos loader (v2026.05.29, built in CI) as `System32\OpenCL.dll` with
`COPYFLG_NO_OVERWRITE`: an `OpenCL.dll` already there is left alone, and
any Khronos loader finds this ICD the same way.

Only ARM64 processes get OpenCL. An x64 process reads the same
`OpenCLDriverName`, cannot load an ARM64 DLL, and the loader goes on
without it; there is no `OpenCLDriverNameWow`. An x64 build behind an
ARM64X forwarder, as `maligl.dll` is, is the way to add them later.

## 4. The kernel driver side

Nothing changes. rusticl's work reaches the GPU the way GL compute does:
Panfrost builds Bifrost compute jobs into a job chain and submits it with
no `PANFROST_JD_REQ_FS`, and the shim (`win32-compat/malikm_drm.c`,
`c[1].slot = (requirements & PANFROST_JD_REQ_FS) ? 0 : 1`) sends it to job
slot 1, the vertex/tiler/compute slot, as one `D3DKMTRender`. maliwddm
does not look inside a chain (`command.c` checks only `Slot <= 1` and a
non-zero chain address). The OpenCL process opens the adapter itself
through D3DKMT, as an OpenGL process does: there is no Direct3D runtime
device in it.

What the KMD does mean for OpenCL:

- **Long kernels are reset.** Windows' TDR delay is 2 s by default. A
  dispatch that runs longer is a TDR: the KMD hard-stops slot 1 and the
  context is lost (the program gets `CL_OUT_OF_RESOURCES` or a lost
  device). Panfrost does not split a dispatch by time. Programs that run
  long kernels need them split, or `TdrDelay` raised for testing.
- **Compute is not preempted.** `TimeSlice` soft-stops only slot 0
  (fragment) chains (`command.c`: `if (A->RunningSlot != 0 ...) return`),
  so while a kernel runs, DWM's frames wait behind it: the desktop
  stutters during long dispatches.
- Buffers are allocations like any other, physical-mode (WDDM 2.0 or 1.3,
  as the GL ICD). No SVM (rusticl's SVM needs Linux `mmap` placement).

## 5. The Mali as a pipe-loader device (0101)

rusticl asks `pipe_loader_probe()` for devices and keeps those whose
driver name is in `RUSTICL_ENABLE`. On Windows only the software backend
is built (no libdrm), which gives `swrast` (softpipe on a null winsys).
0101 adds `pipe_loader_malikm.c`, built on Windows when Panfrost is:

- probe: `malikm_open()` (the shim: `\\.\MaliG52` for malikm, else the
  maliwddm adapter through D3DKMT), and if that gives an fd, one device
  of type `PIPE_LOADER_DEVICE_PLATFORM` named `panfrost` (so OpenCL calls
  it a GPU). `MALIKM_FAKE=1` gives the shim's in-process emulation, for
  CI, as for the GL ICD.
- create_screen: `panfrost_drm_screen_create(fd, config)`, the call the GL
  ICD's `wgl_panfrost_create_screen` makes, with Panfrost's driconf.
- off switches: `MALIKM_OPENCL=0`, or the DWORD
  `HKLM\SOFTWARE\wddm-mali-g52\OpenCL` = 0. Either makes the probe find
  nothing: the rusticl platform is there with no device.

0101 to 0105 are in `mesa-patches-cl/`, which only `opencl.yml`
applies (after main's `mesa-patches/`, so the numbers never collide). The D3D UMD and the GL ICD are built from exactly what they were
built from before (mesa.yml is not triggered by this branch).

## 6. What was built (branch `opencl-rusticl`)

| File | What |
|---|---|
| `.github/workflows/opencl.yml` | Linux job: cltest on PoCL (checks the test against a known-good OpenCL), libclc SPIR-V. Windows ARM64 job: the dependencies (cached), Mesa 26.2 with mesa-patches and mesa-patches-cl, `-Dgallium-rusticl=true -Dgallium-drivers=softpipe,panfrost -Dmesa-clc=enabled -Dllvm=enabled -Dshared-llvm=disabled -Dstatic-libclc=all -Db_vscrt=mt`, the ICD's exports and imports checked, the Khronos loader, clinfo and cltest built, tests on the runner. Artifact `malicl-arm64` |
| `mesa-patches-cl/0101-*` | the pipe-loader backend above |
| `mesa-patches-cl/0102-*` | rusticl on Windows: libc bindings, `dlsym`, `.def`, log symbols |
| `mesa-patches-cl/0103-*` | `mesa_clc` writes SPIR-V in binary mode (text mode broke libpan) |
| `mesa-patches-cl/0104-*`, `0105-*` | `winsys_handle.handle` and the GL export are HANDLEs on Windows (no GL sharing) |
| `.github/opencl/bindgen-unsigned-enums.py` | bindgen wrapper: C enums typed unsigned as on Linux, which rusticl's Rust assumes (MSVC types them int) |
| `.github/opencl/meson-rust-dedup.py` | one `-l` per library for rustc, which refuses duplicates with modifiers |
| `tools/windows/cltest/cltest.c` | `list` (clinfo-like, short), `vecadd [n]`, `sgemm [n] [iters]` (naive and 16x16-tiled kernels, profiling-event timing, GFLOP/s, CPU check with an fp32 tolerance), `all`; `--platform`, `--device`, `--expect`, `--no-verify` |
| `tools/windows/cltest/cltest.cmd` | the board test plan below, logged to one file |
| `drivers/maliwddm/maliwddm.inf` | `[opencl]` lines: the ICD in the driver store, `OpenCL.dll` to System32 (no overwrite), `OpenCLDriverName` |
| `.github/workflows/ci.yml` | job `cl` finds an opencl.yml run of the same inputs, as `umd` does for mesa.yml; the package takes the ICD from it and signs `malicl_arm64.dll` and `OpenCL.dll`, or, with no such run, removes the INF's `[opencl]` lines and is the package it was before. The release zip gets `opencl\cltest.exe`, `clinfo.exe`, `cltest.cmd` |

Tests on the runner (no Mali there): with no device the platform is
listed with no device and nothing crashes; with
`MALIKM_FAKE=1` the device is the Mali-G52, OpenCL C is compiled to
Bifrost code and jobs are submitted (results not checked: nothing runs
them); `MALIKM_OPENCL=0` hides the Mali. No CPU-checked run on the
runner: rusticl refuses softpipe (it lacks compute state queries), so
kernel results are first checked on the board. The runner's shell is
elevated, and an elevated Khronos loader ignores `OCL_ICD_FILENAMES`, so
CI registers the ICD under `HKLM\SOFTWARE\Khronos\OpenCL\Vendors`.

## CI

`opencl.yml` run 45769902509 (2026-11-26, `eed7d6b`), green.
`malicl_arm64.dll` is 117.6 MB, ARM64, exports `clIcdGetPlatformIDsKHR`,
`clGetPlatformInfo`, `clGetExtensionFunctionAddress`, and imports only
Windows DLLs. On the runner: with no Mali, platform `rusticl`, OpenCL 3.1,
0 devices; with `MALIKM_FAKE=1`, device "Mali-G52 r1 MC3 (Panfrost)",
OpenCL 3.0, OpenCL C 1.2, and vecadd and both sgemm kernels compile
(12 to 16 ms) and are submitted. The emulated device has no timestamps,
so cltest falls back to a queue without profiling and the host clock.
`MALIKM_OPENCL=0` hides it.

## Board test plan

TaishanPi 3M (or CM5-IO), Windows 11 23H2. Use the
`wddm-mali-g52-driver-arm64` artifact of a CI run of this branch whose
INSTALL.txt says "OpenCL ICD: opencl.yml run ..." (not "not in this
build"). Install it as any build (`pnputil /add-driver driver\maliwddm.inf
/install`, reboot), then run, as administrator, from the artifact's
`opencl` folder:

1. `cltest.cmd quick`. Pass: the log shows the `OpenCLDriverName` value
   under the display class key, `where OpenCL.dll` finds
   `C:\Windows\System32\OpenCL.dll`, `cltest list` shows platform
   `rusticl` with device "Mali-G52 r1 MC3 (Panfrost)" (GPU), and vecadd
   4096 PASS.
2. `cltest.cmd`. vecadd 16M PASS with a bandwidth figure; sgemm 256, 512
   and 1024, naive and tiled, PASS, with GFLOP/s. 1024 naive is the
   longest dispatch: if it is over 2 s it is a TDR (event 4101 in the
   System log), which is a finding, not a driver bug (section 4).
3. Same with `PAN_MESA_DEBUG=sync` (in the script): passes too, else the
   difference is a synchronisation bug.
4. `MALIKM_OPENCL=0`: no Mali device.
5. While 2 runs, the desktop keeps drawing (slower is expected: compute
   is not preempted); no BSOD, no device removed in dwm.
6. Direct3D and OpenGL are untouched: Edge, the desktop and
   `gltri`/FurMark as before. That OpenCL.dll in System32 now finds a
   device can wake OpenCL code in programs that probe for it (LibreOffice
   Calc, some image editors); note any that change.
7. n=3 for 1 and 2 across cold boots before calling it working
   (`feedback_sample_size_discipline`).

Logs: `cltest-<n>.txt` beside the script, and
`C:\mali\log\<exe>-<pid>-cl.txt` when `C:\mali\log` exists (the shim's
log; `MALIKM_TRACE` for every call, as for GL).

## Expected performance

From what the hardware is and what was measured for D3D and GL, not from
an OpenCL run:

- Peak FP32: 3 shader cores x execution engines per core x 8 lanes x 2
  (FMA) x clock. The G52 has 2 or 3 engines per core depending on the
  configuration; Panfrost reports which as "r1 MC3" plus its core
  features, the board run will say. At the default 594 MHz: 57 GFLOP/s
  with 2 engines, 86 with 3; at 702 MHz (opt-in): 67 or 101.
- SGEMM: the tiled kernel in cltest is a teaching kernel, and Panfrost's
  compiler is not tuned for compute the way Arm's is. 10 to 25 % of peak
  is the expectation: roughly 5 to 20 GFLOP/s at 1024. The naive kernel
  is memory bound and lower. For comparison the A72 cluster (4 x 2.2 GHz
  NEON) peaks at about 70 GFLOP/s, so the GPU is not obviously faster
  than a tuned CPU BLAS for SGEMM; it frees the CPU.
- Memory bound kernels (vecadd): a share of the LPDDR bandwidth the GPU
  can reach, 3 to 8 GB/s expected.
- Per-dispatch overhead: one `D3DKMTRender` per flush plus a fence wait
  that is a poll: PERF.md measured 0.20 to 0.23 ms of submit CPU per job
  for DWM; with the wait, 0.5 to 1 ms per `clFinish` round trip. Small
  kernels in a loop with a finish each are overhead bound.
- Compile time: clang, the translator, NIR with libclc and the Bifrost
  compiler on the A72s, in every process (the shader cache is off in this
  build): hundreds of ms for a small program, more for the first one in a
  process. cltest prints it as `build: N ms`.

## Risks and what is not done

- No board run: correctness on the real GPU is unknown until step 1
  passes. Bifrost OpenCL has no conformance submission.
- DLL size: clang and LLVM inside (the CI log prints it; tens of MB),
  added to the driver package.
- ARM64 only (section 3). The `OpenCL.dll` put in System32 is ARM64 too, so
  an x64 program that loads it gets a load failure, as it would with no
  `OpenCL.dll` at all. No GL sharing (`cl_khr_gl_sharing`), no D3D11
  sharing, no SVM.
- TDR and no compute preemption (section 4).
- Default installed: any program that probes OpenCL now finds the Mali.
  `HKLM\SOFTWARE\wddm-mali-g52\OpenCL` = 0 turns that off without
  uninstalling.

## Merge

Not merged to main. The package change is additive for Direct3D and
OpenGL: their DLLs are built from the same inputs, the INF only adds
files and one value, and a build without an OpenCL ICD is the old package
byte for byte apart from the INF comments. But it is not "cannot affect"
anything: it puts an `OpenCL.dll` in System32 that programs will now load
and that leads them to a GPU path no one has run, inside processes such
as office suites. That is a user-visible change on the board, so it waits
for the board test (steps 1 to 6) before main.
