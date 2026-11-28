# maliwddm's emulated vsync period (drivers/maliwddm/vsync_calc.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

vsync-host: test.c ../../drivers/maliwddm/vsync_calc.h
	$(CC) $(CFLAGS) -o $@ test.c

run: vsync-host
	./vsync-host

clean:
	rm -f vsync-host
