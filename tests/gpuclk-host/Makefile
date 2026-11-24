# maliwddm's GPU clock switch (drivers/maliwddm/gpuclk_calc.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

gpuclk-host: test.c ../../drivers/maliwddm/gpuclk_calc.h
	$(CC) $(CFLAGS) -o $@ test.c

run: gpuclk-host
	./gpuclk-host

clean:
	rm -f gpuclk-host
