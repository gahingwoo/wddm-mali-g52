# maliwddm's source modes and VOP2 window scaling (drivers/maliwddm/scale.h)
# on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra

modes-host: test.c ../../drivers/maliwddm/scale.h
	$(CC) $(CFLAGS) -o $@ test.c

run: modes-host
	./modes-host

clean:
	rm -f modes-host
