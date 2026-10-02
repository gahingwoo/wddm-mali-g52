# M5.1 on the board, 2026-10-02

malidod, the display-only driver, drives the CM5-IO's HDMI output under
Windows 11 23H2: Started, VidPN committed with the firmware's 1920x1080 mode
(`CommitStatus` 0), `DxgkDdiPresentDisplayOnly` called, and the desktop on the
local screen follows window moves. Registry after the session:

    Started                1
    FbWidth/FbHeight       0x780 x 0x438, pitch 0x1e00, format 0x16 (X8R8G8B8)
    CommitStatus           0
    Seen_PresentDisplayOnly 0
    Presents               1   (logged on the first and every 1024th)

## What it took (five runs; each run's output is in this directory)

1. Started, then code 43 with no DDI trace. Added a per-DDI trace.
2. `QAI_1 = 0xC0000023`: DRIVERCAPS answered STATUS_BUFFER_TOO_SMALL.
   dxgkrnl sizes it for the declared interface version (0x220 bytes for WIN8);
   the 26100 WDK's `sizeof(DXGK_DRIVERCAPS)` is larger. Fill what fits.
3. DRIVERCAPS accepted, code 43 still, no further DDI calls: the contents.
   WDDMVersion 0 (from a DXGK_DRIVERCAPS note for WIN7+ drivers) and no
   SupportSmoothRotation; Basic Display reports WDDMv1_2 with SupportNonVGA
   and SupportSmoothRotation, and so does malidod now.
4. Started for the first time; EnumVidPnCofuncModality failed with
   STATUS_GRAPHICS_INVALID_FREQUENCY and the screen went black (RDP fine).
   Target modes now leave the frequencies unspecified, as Basic Display does.
5. Started, first commit with no pinned mode (0x401E0307, informational),
   second commit pinned (0); presents reach the framebuffer.

Recovery that worked: RDP into the board while the local screen was black.
