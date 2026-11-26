# video/: the user-mode driver's VDPU383 backend

Built into maliumd (mesa-patches 0078 and 0081; mesa.yml copies this
directory to `src/gallium/frontends/d3d10umd/video`). The design and the
board test are in [docs/VIDEO-DECODE.md](../docs/VIDEO-DECODE.md).

- `vdec.c`, `vdec.h`: DXVA H.264, HEVC Main, VP9 profile 0 and AV1 Main
  (8 bit) pictures decoded through vdpu383.sys's bring-up interface, one
  decoder surface per DXVA surface index; each call an escape to maliwddm
  (`vdec_kmd.h`, docs/CHROMIUM-VIDEO.md C2), or straight to the device when
  that path is off; a fake decoder for CI
  (`D3D10UMD_VDEC_FAKE=1`); a register log (`D3D10UMD_VDEC_REGLOG=<file>`)
  that tests/vdec-host and mesa.yml compare with vdpu383test's.
- `vdec_kmd.h`: the escape (`MW_ESCAPE_VDEC_CALL`) and the check maliwddm
  makes of it (`drivers/maliwddm/vdecproxy.c`).
- `vdpu383/`: copied unchanged from
  [woa-rk3576](https://github.com/gahingwoo/woa-rk3576) at c89b79d (AV1
  frame_refs[i].Index read as a slot; interface version 3, imports);
  `vdpu383_ioctl.h` and `vdpu383_check.h` (interface 4, sessions) at
  41e5de1 (woa-rk3576 `drivers/video/vdpu383/lib`, `vdpu383_ioctl.h`, `vdpu383_check.h`
  and `vdpu383_import.h`:
  the driver maps buffers through the decoder's IOMMU by default, and with
  version 3 imports the pages of a maliwddm allocation for zero copy;
  `vdpu383_import.h` is the kernel interface maliwddm's vdecexport.c
  implements). H.264 and HEVC are the builders that
  decoded conformance streams bit-exact on the TaishanPi 3M (2026-11-24,
  2026-11-25); VP9 and AV1 have not run on the board. Keep the files
  identical to that tree; sha256 at the copy:

```
27bd92dc0dc14c883c9ed948dab5f1ce671722f4a9c6bf244a53e9a1aa75e76a  lib/v383_dxva.h
7ff68fd43d3effd1c69029eef754a1baeef7a5cb99513dfeb6267b058b923c3c  lib/v383_dxva_hevc.h
64b298485553a076048155f49d963c850e12feec9b923c7cbc00324dfd2544d6  lib/v383_dxva_vp9.h
ff8de91ad7402abd08b56c62e2af945d73e45a7791b933a781d02df880a8fa7d  lib/v383_dxva_av1.h
4f700310cb88d9600b90880b693e82df5b6c3fc66cbcdbbb3022b3d3a68db6e5  lib/vdpu383_h264.c
463c6df35b8952eb2c0d431ff4d40109a61d695a85b30654889d46686b1f98e7  lib/vdpu383_h264.h
2b008ae34c932bdd3c92eb66da7931ea4c69a9962f3e8c8cbedc57fc6a2861ed  lib/vdpu383_cabac.c
5b1b7d7c56c6785222b9c5e170d800fe020aaf681587ae921ae3f7d799c88feb  lib/vdpu383_hevc.c
c35a4b745bc7c5f18884eaf64517c5f2994ac8c7f7fa025514018ffb87d41677  lib/vdpu383_hevc.h
e55453b5b7769294f5180fc1893a7442c92fc8685fc0853166daf6aa50f4a89c  lib/vdpu383_hevc_cabac.c
c383fb96f8e9f1a0b6fcd026e344789a017e21e3872323d2683c95952a2d1613  lib/vdpu383_vp9.c
58469fc5a6037c6dd0300256ef4731db841078798ac93f5913205812b59e0dc4  lib/vdpu383_vp9.h
e0e62f53651b80e9009b41df1e83958067f84cd3b0ded9ef6afaf00d3d698568  lib/vdpu383_vp9_probs.c
10eca42edffed73a2869bbbf57f1732d353742ba9a341829d8313d0f1eebc007  lib/vdpu383_av1.c
81d91d1a583f3a8f336f9d81051fc49c27141b4e8e26f3f590a7e99e5d633681  lib/vdpu383_av1.h
a4294a22a13b5fa5eadbd1ab5ec8452a7a89e6937bf318a72d7f1abb0a9976d2  lib/vdpu383_av1_cdf.c
7be03e75a7bba5abf6309e8028954811c6f58ce67508a79a71e1e3d70a7ac0ed  vdpu383_ioctl.h
1b56a98a971c4ab9913bec17b21ed94d96cb3636d35d0a668a2c1eb2d588ac9b  vdpu383_check.h
fa934aab5719c7271eab227328871a62f1f8f735fb50560fc46d5cdb4c362760  vdpu383_import.h
```

## Licences of the copied files

Everything here is GPL-2.0-only as a whole. Code that came from elsewhere,
and the licence option it is used under (as woa-rk3576's THIRD_PARTY.md
states it):

| File | From | Licence |
|---|---|---|
| `lib/vdpu383_cabac.c`, `lib/vdpu383_hevc_cabac.c` (tables) | Rockchip MPP | Apache-2.0 OR MIT (file header), used as MIT |
| `lib/vdpu383_h264.c`, `lib/vdpu383_hevc.c` (register layouts) | ported from Rockchip MPP, mpp/hal/rkdec h264d, h265d | Apache-2.0 OR MIT, used as MIT; the file GPL-2.0-only |
| `lib/vdpu383_vp9.c` (registers, buffer handling) | ported from MPP hal_vp9d_vdpu383.c | Apache-2.0 OR MIT, used as MIT; the file GPL-2.0-only. The bit layouts follow MPP's hal_vp9d_com.c (Apache-2.0 only), of which no code is copied |
| `lib/vdpu383_vp9_probs.c` (probability tables) | mainline Linux v4l2-vp9.c, Collabora (values from libvpx) | GPL-2.0 |
| `lib/vdpu383_av1.c` (registers, global record, buffer handling) | ported from MPP hal_av1d_vdpu383.c, hal_av1d_com.c, codec/dec/av1 | Apache-2.0 OR MIT, used as MIT; the file GPL-2.0-only |
| `lib/vdpu383_av1_cdf.c` (default CDFs) | MPP hal_av1d_com.c | Apache-2.0 OR MIT, used as MIT; the file GPL-2.0-only |
| the rest of `lib/`, `vdpu383_ioctl.h`, `vdpu383_check.h` | written for woa-rk3576 | GPL-2.0-only |

## The parsers and test streams

`tools/windows/vdectest/h264parse.*`, `hevcparse.*`, `vp9parse.*`,
`av1parse.*` and `md5.*` are vdpu383test's (woa-rk3576
`tools/vdpu383test`, the same commit), with the include paths changed.

Test streams in `tests/data`, FFmpeg's `testsrc` (FFmpeg 6.1.1), each
frame's I420 output (`ffmpeg -i F -autoscale 0 -f rawvideo -pix_fmt
yuv420p`) with the md5 vdectest prints as `output md5`:

| File | sha256 | Made with | Output md5 |
|---|---|---|---|
| `testsrc-176x144-high.264` | `4889b443...` | `-c:v libx264 -profile:v high -preset medium -bf 2 -g 12 -x264-params keyint=12:min-keyint=12:scenecut=0 -bsf:v h264_mp4toannexb -f h264`, 12 frames | `28b5ac0bb9dbb31571f6692a27b3d31a` |
| `testsrc-176x144-main.hevc` | `ce4c271a...` | `-c:v libx265 -preset medium -x265-params keyint=12:min-keyint=12:scenecut=0:bframes=3:b-adapt=0:b-pyramid=1 -f hevc`, 12 frames (I, P, B, referenced B) | `10f8ca257adc8a71cd23d34050735b1c` |
| `testsrc-176x144-p0.ivf` | `157fbee0...` | `-c:v libvpx-vp9 -b:v 200k -g 12 -auto-alt-ref 1 -lag-in-frames 8 -f ivf`, 12 frames | `58a90238270c1b1cd87f8aabab4563e2` |
| `testsrc-352x288-2tiles.av1.ivf` | `21022137...` | `-c:v libaom-av1 -cpu-used 6 -b:v 300k -g 12 -tile-columns 1 -tile-rows 0 -row-mt 0 -f ivf` at 352x288, 12 frames shown (13 decoded, 4 hidden, 3 shown again, 2 tiles) | `b67b6b2e6f33e3ff19a88ee653571362` |

All made with `-f lavfi -i testsrc=size=WxH:rate=25 -frames:v N -pix_fmt yuv420p`.

## Register logs

`tests/vdec-host/reglog.md5` holds, per test stream, the md5 of the
register log vdpu383test writes with `tests/vdec-host/vdpu383test-reglog.patch`
applied (woa-rk3576 a16bd9a):

    vdpu383test --null decode tests/data/STREAM --dxva-only [--av1-synth 1] --reglog log.txt

The log is each register file with its addresses as buffer and offset
(`strm`, `aux`, `rcb`, `colmv0`, `colmv1`, `sN` the surface of DXVA index
N), then an FNV-1a hash of the auxiliary buffer and of the stream bytes the
decoder is told to read. vdec.c writes the same lines. On 2026-11-25 the
two were identical for every 8-bit stream of the kits: 75 HEVC
conformance streams, 215 VP9 test vectors, 21 AV1 streams, the 6 H.264
conformance streams (the 10-bit ones are refused by the UMD on purpose).
