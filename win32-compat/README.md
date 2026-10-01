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

Not implemented: PRIME (dma-buf) import and export, syncobj fds and timelines
(Panthor only), anonymous and file mappings. They fail with `ENOSYS`.
