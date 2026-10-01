# Plan from M2 on

Written 2026-10-01, after M1. It changes the milestone order in the README:
Mesa's D3D10 frontend turned out to be a software-driver interface, which
gives a way to get Panfrost driving the Mali under Windows before any WDDM
code exists.

## What changed the plan

**Mesa's d3d10umd is a software rasterizer interface, not a hardware UMD.**
It implements the same DDI as WARP: an application loads it with
`D3D_DRIVER_TYPE_SOFTWARE` and the DLL's handle. It calls none of the WDDM
runtime callbacks a hardware UMD uses to allocate memory or submit work
(`pfnAllocateCb`, `pfnRenderCb` and the rest), and its `D3DKMT*` entry points
are stubs. Its README: "Currently it only supports SW rasterizers."

Two consequences:

1. It cannot become the desktop's driver as it is. Getting DWM onto the Mali
   still needs a WDDM 2.0 kernel driver (Windows 11's minimum) and a UMD that
   uses the hardware callbacks. That is the last milestone, not the next one.
2. Nothing in it cares which Gallium driver is underneath. With Panfrost
   there, and Panfrost talking to the GPU through a kernel interface of our
   own, D3D11 applications that ask for the software driver render on the
   Mali. That needs no WDDM at all, and it proves the hardest unknown, Panfrost
   working on Windows, first.

Other facts this rests on:

- Mesa 26.2.0 is the last release with d3d10umd (removed 2026-09-30). It
  requires softpipe or llvmpipe to be built alongside; Panfrost can be built
  next to softpipe and the target's screen creation pointed at it.
- Panfrost turns TGSI into NIR itself (`pan_shader.c`, `tgsi_to_nir`), so the
  frontend's TGSI is not a blocker.
- About ten Panfrost files use Linux interfaces (`xf86drm.h`, `mmap`,
  dma-buf): `pan_bo.c`, `pan_device.c`, `pan_resource.c`, `pan_context.c`,
  `pan_mempool.c`, `pan_screen.h`, `lib/kmod/*`, `lib/wrap.h`. The kernel
  interface is already behind `pan_kmod_ops`; a Windows backend replaces the
  Panfrost and Panthor ones.
- GitHub's `windows-11-arm` runner is free for public repositories, so Mesa
  builds natively for ARM64 in CI, and D3D11 programs can run there against
  the software driver as a smoke test (no GPU needed for softpipe).

## M2 result (2026-10-02)

Done. `.github/workflows/mesa.yml` builds Mesa 26.2 natively on
`windows-11-arm`: d3d10umd with softpipe (MSVC), and d3d10umd with softpipe
and Panfrost (clang-cl). The triangle test passes on both; with
`GALLIUM_DRIVER=panfrost` the DLL looks for `\\.\MaliG52`, finds none on the
runner and falls back to softpipe.

What it took, in `mesa-patches/` and `win32-compat/`:

- **clang-cl instead of cl** for the Panfrost build: same MSVC ABI, but it
  accepts the GNU C Panfrost is written in.
- **MSVC-ABI bitfields.** Mixed-type bitfields are laid out differently and
  enum bitfields are signed. The Bifrost hardware encodings are now packed
  explicitly (0003, checked against GCC's layout on Linux in CI), every
  enum-typed bitfield is unsigned (0011; signed `bi_size` crashed the compiler),
  and `bi_index`'s hash key is built from its fields (0007).
- **libpan without LLVM on Windows**: the Linux job generates the SPIR-V and
  bindings; two Python stand-ins for `mesa_clc` and `vtn_bindgen2` copy them
  in; `panfrost_compile` runs natively (0010).
- **POSIX and libdrm shims** (`win32-compat/`): every kernel call fails with
  ENOSYS for now. In M3 they become DeviceIoControl calls with Linux Panfrost
  semantics, so `panfrost_kmod.c` stays as it is.
- `PACKED` must be real under clang-cl (`-DHAVE_FUNC_ATTRIBUTE_PACKED`):
  libpan's kernel argument structs are 12 bytes on the GPU side.

## Milestones

| | Milestone | Where it runs | Done when |
|---|---|---|---|
| M2.1 | Mesa 26.2 d3d10umd + softpipe built for Windows ARM64 | CI | `libgallium_d3d10.dll` builds, and a D3D11 test program renders a triangle through it on the runner and checks pixels |
| M2.2 | Same DLL with Panfrost inside, Windows kmod backend stubbed | CI | it builds and links; Panfrost reports "no device" cleanly at run time |
| M3.1 | Kernel interface: `m1probe` grows into a KMDF driver with a Panfrost-like IOCTL ABI | board | a user-mode test allocates a buffer, maps it, submits the M1 WRITE_VALUE job through the IOCTLs, and reads the value |
| M3.2 | Windows kmod backend in Mesa calls those IOCTLs | board | the D3D11 test program clears a render target on the Mali and reads back the colour |
| M4 | First triangle on the Mali | board | the same test program's triangle, pixels checked |
| M5 | WDDM 2.0 driver, hardware UMD, DWM on the Mali | board | the desktop composites on the GPU |

M2 is CI only; the board is not needed until M3.

## M2.1 in detail

- A workflow on `windows-11-arm`: MSVC (already on the image), Python with
  `meson`, `ninja`, `mako`, `pyyaml`, `packaging`, and `winflexbison` for
  flex/bison.
- Clone Mesa at tag `mesa-26.2.0`, apply `mesa-patches/*.patch` from this
  repo, configure with
  `-Dgallium-drivers=softpipe -Dgallium-d3d10umd=true -Dvulkan-drivers=[] -Dllvm=disabled -Dplatforms=windows`.
- `tests/d3d11-triangle`: a small C++ program that creates a D3D11 device with
  `D3D_DRIVER_TYPE_SOFTWARE` on the built DLL, renders a triangle into a
  texture, copies it to a staging texture and checks a few pixels. Built in the
  same job and run there.
- Artifacts: the DLL and the test program, so the same pair can be tried on
  the board.

## M2.2 in detail

- Patches, kept small and one concern each, so they can be rebased if Mesa is
  ever moved past 26.2: guard the Linux includes, route `mmap` and dma-buf use
  through `pan_kmod`, add a `windows` kmod backend that only returns "no
  device" for now, let the d3d10umd target create a Panfrost screen when
  asked (environment variable `MALI_D3D10_DRIVER=panfrost`, softpipe stays
  the default).
- `-Dgallium-drivers=softpipe,panfrost`.

## M3.1: the kernel interface

Modelled on the Linux Panfrost uAPI so the Windows kmod backend stays a thin
translation:

| IOCTL | Linux equivalent | Notes |
|---|---|---|
| `GET_PARAM` | `DRM_IOCTL_PANFROST_GET_PARAM` | GPU_ID, core masks, features: values M1 already reads |
| `CREATE_BO` | `CREATE_BO` | allocates pages, maps them into the process's GPU address space |
| `MAP_BO` | `MMAP_BO` | maps the BO into the calling process (MDL) |
| `FREE_BO` | `GEM_CLOSE` | |
| `SUBMIT` | `SUBMIT` | one job chain on slot 0 or 1, the BO list |
| `WAIT_BO` | `WAIT_BO` | completion via the job interrupt instead of M1's polling |

One process, one GPU address space (AS0) to start; per-process address spaces
come with WDDM. Interrupts replace polling: the job, MMU and GPU interrupts are
already in the DSDT.

## Risks

1. **Panfrost may lean on Linux more deeply than ten files suggest** (Mesa's
   `util/` is portable, but the BO cache and synchronization assume dma-buf
   and syncobj semantics). M2.2 will show how deep; it is CI only, so finding
   out is cheap.
2. **The software-driver path costs a copy per frame**: the result is
   presented through GDI from CPU-visible memory. Fine for proving the GPU
   works, not a substitute for M5.
3. **d3d10umd is D3D10 / feature level 10_0 and unmaintained.** It is a
   stepping stone; M5 needs a hardware UMD anyway.
4. **Mesa 26.2 on the `windows-11-arm` image.** Mesa's own Windows CI is x64;
   ARM64 MSVC builds of Mesa exist (mesa-dist-win), but not of these drivers.
