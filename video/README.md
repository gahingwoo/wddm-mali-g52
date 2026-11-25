# video/: the user-mode driver's VDPU383 backend

Built into maliumd (mesa-patches 0078; mesa.yml copies this directory to
`src/gallium/frontends/d3d10umd/video`). The design and the board test are
in [docs/VIDEO-DECODE.md](../docs/VIDEO-DECODE.md).

- `vdec.c`, `vdec.h`: DXVA H.264 pictures decoded through vdpu383.sys's
  bring-up interface, one decoder surface per DXVA surface index; a fake
  decoder for CI (`D3D10UMD_VDEC_FAKE=1`).
- `vdpu383/`: copied unchanged from
  [woa-rk3576](https://github.com/gahingwoo/woa-rk3576) branch
  `vdpu383-v2` at fb2a730 (`drivers/video/vdpu383/lib` and
  `vdpu383_ioctl.h`), the register builder that decoded the H.264
  conformance streams bit-exact on the TaishanPi 3M on 2026-11-24. Keep
  them identical to that tree; sha256 at the copy:

```
7446e5749846039456f451dd11095970b99aba5737d7049bd286f5d2821f1060  lib/vdpu383_h264.c
2b008ae34c932bdd3c92eb66da7931ea4c69a9962f3e8c8cbedc57fc6a2861ed  lib/vdpu383_cabac.c
463c6df35b8952eb2c0d431ff4d40109a61d695a85b30654889d46686b1f98e7  lib/vdpu383_h264.h
27bd92dc0dc14c883c9ed948dab5f1ce671722f4a9c6bf244a53e9a1aa75e76a  lib/v383_dxva.h
d41b49fbd988cca4b1290f5507f533d2bbc97f27bc1f6499ef68de636c814319  vdpu383_ioctl.h
```

`tools/windows/vdectest/h264parse.*` and `md5.*` are vdpu383test's, from
the same commit, with the include paths changed.

`tests/data/testsrc-176x144-high.264`: FFmpeg's `testsrc`, 12 frames,
x264 High profile with B frames (CABAC, 8x8 transform), made with

    ffmpeg -f lavfi -i testsrc=size=176x144:rate=25 -frames:v 12 -pix_fmt yuv420p \
      -c:v libx264 -profile:v high -preset medium -bf 2 -g 12 \
      -x264-params keyint=12:min-keyint=12:scenecut=0 -bsf:v h264_mp4toannexb -f h264 out.264

sha256 4889b4437a546e1721a4f9131b05075b8bee5ec0e35829f923a91d71e58f1690.
Its decoded I420 output (`ffmpeg -i out.264 -f rawvideo -pix_fmt yuv420p`)
has md5 28b5ac0bb9dbb31571f6692a27b3d31a; vdectest prints the md5 of what
it decoded as `output md5`.
