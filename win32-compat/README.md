# win32-compat

POSIX and libdrm headers that Panfrost includes unconditionally, for the
Windows build only (added to the include path by `.github/workflows/mesa.yml`).

Everything that would talk to a kernel driver (`drmIoctl`, syncobjs, PRIME,
`mmap` of a buffer, `poll`) fails with `ENOSYS` for now, so Panfrost reports no
device. In M3 these become calls into the Mali KMDF driver through
DeviceIoControl, with the same semantics as Linux's Panfrost uAPI, so
Panfrost's own Linux kernel backend (`panfrost_kmod.c`) can stay as it is.
