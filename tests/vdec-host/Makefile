# The UMD's video backend (video/vdec.c) and the register builder with the
# fake decoder, on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra

V = ../../video
P = ../../tools/windows/vdectest

vdec-host: test.c $(V)/vdec.c $(V)/vdec.h $(V)/vdpu383/lib/vdpu383_h264.c $(V)/vdpu383/lib/vdpu383_cabac.c $(P)/h264parse.c
	$(CC) $(CFLAGS) -o $@ test.c $(V)/vdec.c $(V)/vdpu383/lib/vdpu383_h264.c $(V)/vdpu383/lib/vdpu383_cabac.c $(P)/h264parse.c

run: vdec-host
	./vdec-host ../data/testsrc-176x144-high.264

clean:
	rm -f vdec-host
