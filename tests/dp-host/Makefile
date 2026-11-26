# maliwddm's DisplayPort over USB-C arithmetic (drivers/maliwddm/dp_calc.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

dp-host: test.c ../../drivers/maliwddm/dp_calc.h ../../drivers/maliwddm/dual_calc.h ../../drivers/maliwddm/hdmi_calc.h
	$(CC) $(CFLAGS) -o $@ test.c

run: dp-host
	./dp-host

clean:
	rm -f dp-host
