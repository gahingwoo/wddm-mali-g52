# maliwddm's two-display decisions (drivers/maliwddm/dual_calc.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

dual-host: test.c ../../drivers/maliwddm/dual_calc.h ../../drivers/maliwddm/hdmi_calc.h
	$(CC) $(CFLAGS) -o $@ test.c

run: dual-host
	./dual-host

clean:
	rm -f dual-host
