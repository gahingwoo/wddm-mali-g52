# maliwddm's GPU temperature fallback (drivers/maliwddm/thermal_calc.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

thermal-host: test.c ../../drivers/maliwddm/thermal_calc.h
	$(CC) $(CFLAGS) -o $@ test.c

run: thermal-host
	./thermal-host

clean:
	rm -f thermal-host
