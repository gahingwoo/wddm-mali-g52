# Results

One folder per milestone or board session, oldest first. A folder's
`README.md`, where there is one, is the summary; the other files are the logs
and images it rests on.

| Folder | Date | Board | What |
|---|---|---|---|
| [m1/](m1/) | 2026-10-01 | CM5-IO | The Mali runs one job under Windows (`m1probe`) |
| [m3/](m3/) | 2026-10-02 | CM5-IO | `malikm` alone, then Panfrost clearing a render target through it |
| [m4/](m4/) | 2026-10-02 | CM5-IO | The first triangle, byte-identical to softpipe |
| [m5.1/](m5.1/README.md) | 2026-10-02 | CM5-IO | `malidod`, the display-only driver, drives HDMI; the four runs it took |
| [m5.2-board-2026-10-02/](m5.2-board-2026-10-02/) | 2026-10-02 | CM5-IO | Runs 1 to 7 of the full driver in installed Windows, before the WinPE runs |
| [m5.2-step-b/](m5.2-step-b/README.md) | 2026-10-02 | CM5-IO, WinPE | A job submitted through the WDDM driver; WinPE runs 1 to 15 |
| [m5.2-step-c1/](m5.2-step-c1/README.md) | 2026-10-02 | CM5-IO, WinPE | The triangle through the WDDM driver |
| [m5.2-step-c2/](m5.2-step-c2/README.md) | 2026-10-03 | CM5-IO, WinPE | The triangle as a hardware device, Mesa loaded by the runtime as the UMD |
| [m5.2-pe-whea-2026-10-03/](m5.2-pe-whea-2026-10-03/README.md) | 2026-10-03 and 2026-10-17 | CM5-IO, WinPE | A shutdown crash traced to USB, and the job-fault fix |
| [tspi3m-2026-11-13/](tspi3m-2026-11-13/README.md) | 2026-11-13 | TaishanPi 3M | Step (d): the sign-in screen and the desktop composed on the Mali |
| [tspi3m-2026-11-14/](tspi3m-2026-11-14/README.md) | 2026-11-14 | TaishanPi 3M | Speed, memory and hitches measured; scan-out by the VOP2; x64 and x86 programs; OpenGL; the GPU's clock |

File names inside the older folders still carry their milestone prefix
(`m5.2-pe-run15-PASS.txt` and so on), because commit messages and the
summaries refer to them by those names.
