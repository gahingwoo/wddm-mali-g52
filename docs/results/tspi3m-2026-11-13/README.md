# 2026-11-13: the Windows desktop composed on the Mali-G52

Board: LCKFB TaishanPi 3M (RK3576, 4 GB), Windows 11 23H2 (22631) installed
on the eMMC, 2560x1440 over HDMI. Firmware: edk2-rk3576 `00b82c4`, which
powers the GPU and declares it as `ACPI\RKCP7402`. The work was done on the
branch `umd-file-log`; `main` has it up to the state described here, and the
branch carries on from there.

![The Windows 11 desktop](desktop-on-mali.jpg)

Later the same evening: signed in, and the desktop came up on the first try.
Wallpaper, desktop icons with their labels, the taskbar, and Task Manager as
a window ([photo](task-manager-on-mali.jpg)), all drawn by `explorer.exe`,
`Taskmgr.exe` and the others through Panfrost and composed by `dwm.exe`.
Five processes had the UMD loaded; 44803 render jobs had run, none failed.

What does not work on that desktop:

- The Start menu and Search. `StartMenuExperienceHost.exe` and
  `SearchHost.exe` crash (four times each in the first minutes) after
  `tgsi_exec.c:1518: reg->Indirect.File == TGSI_FILE_ADDRESS`. That file is
  the TGSI interpreter, which Panfrost does not use, so these two are not
  rendering on the Mali at all. Found and fixed later that evening (item 14
  below): the three shell hosts now render on the Mali and stay up. Whether
  the Start menu, Search and the tray look right has not been looked at.
- Parts of the taskbar: the clock and tray on the right are missing and a
  blank flyout shows near the middle.
- The error box in the photos is not about graphics.
  `SecurityHealthSSO.dll` on this installation is 1418216 bytes of zeros,
  left by one of the day's hard resets or by the eMMC clock fault fixed that
  morning.
- Speed, as below.

The sign-in screen, an hour earlier:

![The Windows sign-in screen](logon-screen-on-mali-with-text.jpg)

What is on the monitor in that photo:

- `LogonUI.exe` renders the sign-in screen with Panfrost into a shared
  surface.
- `dwm.exe` opens that surface, composites it with Panfrost into its own
  swap chain, and presents by flipping.
- `maliwddm` takes each flip and copies the new primary to the firmware's
  framebuffer at the emulated vertical sync.

The wallpaper, the account picture, the user name, the password box with the
typed dots, the icons in the corner and the mouse pointer are all right.

It is slow, about 1 to 2 flips a second with the sign-in screen up and 5 to 6
with an empty desktop. Every frame is read back from Panfrost's buffer and
copied twice on the CPU at 2560x1440.

The first version of this picture, earlier the same evening, had no text and
no account picture: [logon-screen-on-mali-no-text.jpg](logon-screen-on-mali-no-text.jpg).
Both causes were found with a Direct2D test that runs over SSH (item 12
below).

An earlier photo, before `LogonUI.exe` got as far as presenting, shows DWM's
empty desktop and the pointer: [dwm-on-mali-cursor-only.jpg](dwm-on-mali-cursor-only.jpg).

## Counters at that point

From `m5test` and the driver's trace values, on one boot:

- 40473 render jobs done, 0 failed, 0 timed out.
- 12369 flips, 12365 copied to the framebuffer.
- `dwm.exe` one process since it was last started; `LogonUI.exe` one process
  and no crash on four starts. Before the shared surface work it crashed
  every 8 seconds.
- No assertion and no unimplemented entry point in either process's log.

## Before this day

M5.2 step (c) had passed in WinPE: a triangle through the WDDM driver with
Mesa loaded as the UMD. Step (d), DWM in installed Windows, had been tried
once on the CM5-IO on 2026-10-17 and ended with a black screen and a shell
that crash-looped.

## What stood in the way, in the order it was found

Each one was read from a log line, not guessed. The two guesses made along
the way (that `OpenResource` was the first thing missing, and that
`DXGI_STATUS_NO_REDIRECTION` was) were wrong about where things stopped.

1. **16 viewports for a driver that has one.** `dwm.exe` and `LogonUI.exe`
   created a device and then did nothing. They were sitting in an assertion
   box, `pan_context.c:496: num_viewports == 1`, on the secure desktop where
   no key reaches it ([photo](assert-num-viewports.jpg)). Direct3D 10 clears
   all 16 viewport slots when a device is set up and the frontend passed them
   all on.
2. **Assertions that stop a process.** They now go to a file and execution
   continues, so one run lists everything it hits.
3. **Fragment position.** `tgsi_to_nir` asserted on a position input when
   the driver wants a system value, although it had already made the right
   variable.
4. **No SAMPLE opcodes.** The frontend emits Direct3D 10 style sampling,
   with the sampler and the sampler view as separate registers.
   `tgsi_to_nir` only knew the combined TEX family, so no textured shader
   had ever gone through Panfrost here. Eight opcodes added.
5. **Constant buffers.** Panfrost pushed uniform words only from user
   buffers; Direct3D 10 constant buffers are resources. Every shader ran
   without its constants (207 assertions in one run).
6. **Primaries the runtime knows.** Every present returned `E_INVALIDARG`:
   the shim presented an allocation of its own. Primaries are now allocated
   through the runtime with the resource handle, and their allocations
   rotate with `RotateResourceIdentities`.
7. **Two caps inside the wrong `#if`.** `SectionBackedPrimary` and
   `FlipOnVSyncMmIo` had been added inside `#if MW_RENDER_ONLY` and were 0
   in the only build there is.
8. **`SetDisplayMode` was a stub.** DXGI calls it to move the display to the
   swap chain. Without it dxgkrnl answered every flip with
   `STATUS_GRAPHICS_PRESENT_MODE_CHANGED`. The frontend's own log said so
   1887 times (`_SetDisplayMode XXX`) and I looked at the kernel's status for
   an iteration before reading it.
9. **The driver refused flips.** `MwPresent` wanted DMA private data, which a
   flip does not have (`Fail_Present 0xC000000D`).
10. **Two shaders `LogonUI.exe` needs.** An indirect read of plain
    temporaries, and `SV_VertexID`, which Bifrost's compiler has no
    intrinsic for in its zero-based form.
11. **Shared surfaces.** A resource created with
    `D3D10_DDI_RESOURCE_MISC_SHARED` is now backed by an allocation the
    runtime owns, `OpenResource` builds a resource on the opened allocation,
    and a shared surface is presented as it is. The kernel driver needed no
    change: one GPU address space and `OpenAllocation` were enough.
12. **Direct2D's text and bitmaps.** `tools/windows/d2dtext.cpp` draws a
    rectangle, an ellipse, two lines of text and a bitmap with Direct2D and
    saves the frame, on the Mali or on WARP. On the Mali only the rectangle
    was right ([before](d2dtext-mali-run-43389770021.png),
    [WARP](d2dtext-warp.png)). Two causes, both in `tgsi_to_nir`:
    - Vertex inputs were all declared float, so Bifrost converted integer
      elements to float and the shader used the float's bits as an integer.
      Direct2D indexes a constant buffer with such an input, one entry per
      primitive, and found nothing past entry 0. That lost the ellipse and
      the bitmaps.
    - Nothing told Panfrost which sampler views are texel buffers, so a
      pixel shader that fetches from one had no attribute array and read
      zero. Direct2D keeps glyph coverage in a buffer. That lost the text;
      `tools/windows/d3dparts.cpp` found it, one feature per draw.

    With both fixed the Mali's frame has the same number of text pixels as
    WARP's (2089 and 2290 for the two lines) and 72 pixels of 131072 differ
    by more than 8, at the edge of the ellipse and in the filtered bitmap
    ([after](d2dtext-mali-run-43394072587.png)).
13. **`explorer.exe` crashed inside the UMD.** `0xc0000005` at
    `maliumd.dll+0x17e9e0`, which the build's `.pdb` resolves to
    `pan_resource_afbcp_update`. The frontend gave an input slot with no
    vertex buffer a user buffer of four constant floats; Panfrost takes no
    user vertex buffers and read the pointer as a resource. Such a slot now
    gets a real buffer of zeros.
14. **One device per process was an assumption.** The shim between Panfrost
    and the runtime kept one BO table, one command buffer and one set of
    runtime callbacks per process, rebound by every `CreateDevice`.
    `explorer.exe` makes a device per window and more (five in 17 s), shares
    surfaces between them and destroys them; its log had
    `panfrost_batch_submit failed: 5` 97 times. The shim now keeps that
    state per runtime device, and each device has its own Panfrost screen.
15. **AppContainer processes got softpipe.** The fd the shim hands Mesa in
    place of a DRM device was `NUL`, and the shell's XAML hosts
    (`StartMenuExperienceHost`, `SearchHost`, `ShellExperienceHost`) may not
    open `NUL` (`errno 13, error 5`). They fell back to softpipe, whose TGSI
    interpreter cannot index through a temporary, and failed fast. The
    placeholder is now a pipe end where `NUL` does not open. Nothing had
    logged the reason; the bind and screen creation steps do now.

The commits on the branch say what was observed for each.

## How it was debugged

- The UMD writes every entry point, every shader's TGSI and its own messages
  to `C:\mali\log\<exe>-<pid>.txt` while that directory exists, and
  assertions to `<exe>-<pid>-assert.txt`.
- A new UMD does not need a reboot: rename `System32\maliumd.dll`, copy the
  new one, end `LogonUI.exe` and `dwm.exe`; Windows starts both again.
- The kernel driver keeps counters and its last failing call per DDI under
  the device's `Device Parameters\maliwddm` key.
- dxgkrnl events are decoded with `tools/etw/`.
- A fault inside the UMD is written to the same log with its offset in the
  DLL, the link register and the stack words that point into the DLL;
  `llvm-symbolizer --obj=libgallium_d3d10.dll --relative-address` with the
  `.pdb` from the same CI run turns those into functions and lines.
- `tools/windows/d2dtext.cpp` and `d3dparts.cpp` render off screen on the
  Mali or on WARP and check pixels, over SSH.
- A second Windows on the same eMMC is the default boot entry while a new
  kernel driver is tried, with a one-time boot into the system under test.

When something stops, in this order: `XXX` and `SetError` lines in the UMD
log, `Fail_*` in the driver's key, assertions, the Application event log,
then dxgkrnl events.

## Open

- The Start menu, Search, the tray and File Explorer's toolbar: their
  processes now render on the Mali without an error in their logs; what
  they look like has not been checked.
- Speed: render into the primary instead of reading back and copying, then
  scan out the primary directly.
- With the display asleep, `dwm.exe` closed the shared surfaces two seconds
  after opening them (seen once).
- `DXGI_STATUS_NO_REDIRECTION` is still returned from `CreateDevice`; what
  windowed applications need has not been looked at.
- Not run on the CM5-IO.

## Files

Photos of the monitor are next to this file. Everything else is in
[logs/](logs/):

| Files | What |
|---|---|
| `umd-*.txt`, `traces-after-sample.txt`, `dwm-trace-with-tgsi.txt`, `logonui-trace-shared-resource.txt` | UMD logs at each stage |
| `dxgkrnl-*.txt` | decoded dxgkrnl events |
| `m5test-after-clean-boot.txt`, `maliwddm-trace-after-clean-boot.txt`, `session1-processes.txt` | the first clean boot with the driver bound |
