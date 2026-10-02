# M5.2 step (c), part 2: Mesa loaded by the D3D runtime as the Mali's UMD, 2026-10-03

**Passed** in WinPE (22621) on the CM5-IO, run 17 ([output](pe-run17.txt)).

maliwddm's INF names `maliumd.dll` (Mesa's `libgallium_d3d10.dll`: d3d10umd
with Panfrost) as the adapter's user-mode driver. `triangle.exe hw` finds the
adapter through DXGI and creates a **hardware** device on it
(`D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, ...)`); the D3D runtime
loads the UMD from the adapter's registry, as it would for any application.

    adapter 0: Rockchip RK3576 Mali-G52 (WDDM) (vendor 0x50434b52 device 0x32303437 ...)
    adapter 1: Microsoft Basic Render Driver (vendor 0x1414 device 0x008c ...)
    device created (hardware), feature level 0xa000
    centre=0xff0000ff (want 0xff0000ff) corner=0xffff0000 (want 0xffff0000) -> PASS

The image ([ppm](triangle-hw.ppm)) is byte-identical to M4's and to the
software-device run. The Mali is DXGI's adapter 0, ahead of Basic Render.
maliwddm's counters over the run (m5test, the triangle as a software device,
then as a hardware device): 17 jobs, 0 failed, 0 faults, 0 resets; 27 aperture
maps and 27 unmaps.

## What is still not "the" UMD

Inside the UMD, Panfrost reaches the KMD through the libdrm shim's own D3DKMT
device and context, not through the runtime's device callbacks
(`pfnAllocateCb`, `pfnRenderCb`, ...). Resources the runtime itself owns
(swap chain buffers, shared surfaces) and Present need the callbacks; that is
the next piece, and d3d10umd creates its gallium screen at OpenAdapter, before
any device callbacks exist.
