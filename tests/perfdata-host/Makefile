# maliwddm's answers to dxgkrnl's performance queries (drivers/maliwddm/perfdata_calc.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

perfdata-host: test.c ../../drivers/maliwddm/perfdata_calc.h ../../drivers/maliwddm/thermal_calc.h
	$(CC) $(CFLAGS) -o $@ test.c

run: perfdata-host
	./perfdata-host

clean:
	rm -f perfdata-host
