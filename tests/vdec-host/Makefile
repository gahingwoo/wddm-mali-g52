# The UMD's video backend (video/vdec.c) and the register builders with the
# fake decoder, on the host: every codec's test stream, the register files
# compared with vdpu383test --dxva-only's (reglog.md5, README.md). `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra

V = ../../video
L = $(V)/vdpu383/lib
P = ../../tools/windows/vdectest
D = ../data

LIB = $(L)/vdpu383_h264.c $(L)/vdpu383_cabac.c $(L)/vdpu383_hevc.c $(L)/vdpu383_hevc_cabac.c \
      $(L)/vdpu383_vp9.c $(L)/vdpu383_vp9_probs.c $(L)/vdpu383_av1.c $(L)/vdpu383_av1_cdf.c
PARSE = $(P)/h264parse.c $(P)/hevcparse.c $(P)/vp9parse.c $(P)/av1parse.c $(P)/md5.c

vdec-host: test.c fakekmd.c fakekmd.h $(V)/vdec.c $(V)/vdec.h $(V)/vdec_kmd.h $(V)/vdpu383/vdpu383_check.h $(LIB) $(PARSE)
	$(CC) $(CFLAGS) -o $@ test.c fakekmd.c $(V)/vdec.c $(LIB) $(PARSE)

# reglog.md5: one line per run, "stream frames md5 [options]".
run: vdec-host
	@set -e; grep -v '^#' reglog.md5 | while read s f m o; do \
	  echo "== $$s $$o"; ./vdec-host $(D)/$$s --frames $$f --reglog-md5 $$m $$o --log reglog-$$s.txt; \
	done
	@# zero copy (V3b): the same streams into a texture of the test's
	@set -e; grep -v '^#' reglog.md5 | while read s f m o; do \
	  echo "== $$s $$o --zero-copy"; ./vdec-host $(D)/$$s --frames $$f $$o --zero-copy --log zc-$$s.txt; \
	done
	@# a refused import asked again later, with a new export (video/vdec.c import_get)
	@set -e; grep -v '^#' reglog.md5 | while read s f m o; do \
	  echo "== $$s $$o --zero-copy --import-fail 1"; \
	  ./vdec-host $(D)/$$s --frames $$f $$o --zero-copy --import-fail 1 --log zf-$$s.txt; \
	done
	@# textures with exactly the picture's rows rounded to 8 (Chromium's 1080)
	@set -e; grep -v '^#' reglog.md5 | while read s f m o; do \
	  echo "== $$s $$o --zero-copy --tex-rows-align 8"; \
	  ./vdec-host $(D)/$$s --frames $$f $$o --zero-copy --tex-rows-align 8 --log zr-$$s.txt; \
	done
	@# through maliwddm (C2): the same register files, then the refusals;
	@# zero copy, and an eviction while imported
	@set -e; grep -v '^#' reglog.md5 | while read s f m o; do \
	  echo "== $$s $$o --kmd"; \
	  ./vdec-host $(D)/$$s --frames $$f --reglog-md5 $$m $$o --kmd --log kmd-$$s.txt; \
	  echo "== $$s $$o --kmd --zero-copy --revoke-at 3"; \
	  ./vdec-host $(D)/$$s --frames $$f $$o --kmd --zero-copy --revoke-at 3 --log kzc-$$s.txt; \
	done

clean:
	rm -f vdec-host reglog-*.txt zc-*.txt zf-*.txt zr-*.txt kmd-*.txt kzc-*.txt vdec-host-reglog.txt
