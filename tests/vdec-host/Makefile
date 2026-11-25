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

vdec-host: test.c $(V)/vdec.c $(V)/vdec.h $(LIB) $(PARSE)
	$(CC) $(CFLAGS) -o $@ test.c $(V)/vdec.c $(LIB) $(PARSE)

# reglog.md5: one line per run, "stream frames md5 [options]".
run: vdec-host
	@set -e; grep -v '^#' reglog.md5 | while read s f m o; do \
	  echo "== $$s $$o"; ./vdec-host $(D)/$$s --frames $$f --reglog-md5 $$m $$o --log reglog-$$s.txt; \
	done

clean:
	rm -f vdec-host reglog-*.txt vdec-host-reglog.txt
