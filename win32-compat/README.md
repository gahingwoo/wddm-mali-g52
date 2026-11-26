# win32-compat

POSIX and libdrm headers that Panfrost includes unconditionally, for the
Windows build only (added to the include path by `.github/workflows/mesa.yml`).

`malikm_drm.c` implements the libdrm calls Panfrost makes, and `mmap`/`munmap`
of a buffer object, as DeviceIoControl calls on `\\.\MaliG52`, the device of
the Mali kernel driver in `drivers/malikm`. The driver keeps Linux Panfrost's
uAPI semantics, so Panfrost's own Linux kernel backend (`panfrost_kmod.c`) runs
as it is. The ABI between the two is `include/malikm_ioctl.h`, which CI copies
next to these headers. `mesa-patches/0015` links the shim into Panfrost's kmod
library.

The same file is the kernel backend of the Vulkan driver (panvk,
`mesa-patches/vulkan/`, docs/VULKAN.md). For it the shim also answers
`drmGetCap`, `drmGetDevice2` and the syncobj ioctls panvk makes directly,
and, when panvk asks for it with `malikm_syncobj_state(1)`, keeps a state per
syncobj (unsignalled, signalled, or pending on a submission) instead of
treating every wait as a wait for idle. The Direct3D UMD and the OpenGL ICD
never turn that on. `sched.h` and `sys/sysmacros.h` are for the Vulkan
runtime and panvk only.

Not implemented: PRIME (dma-buf) import and export, opaque syncobj fds and
timelines (Panthor only), anonymous and file mappings. They fail with
`ENOSYS`.
