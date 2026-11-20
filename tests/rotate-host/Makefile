# maliwddm's rotation (drivers/maliwddm/rotate.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra

rotate-host: test.c ../../drivers/maliwddm/rotate.h
	$(CC) $(CFLAGS) -o $@ test.c

run: rotate-host
	./rotate-host

clean:
	rm -f rotate-host
