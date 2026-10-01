# malikm on the host, against a mock WDK and a simulated G52. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-multichar \
          -D_KERNEL_MODE -Imock -I../../include -pthread
DRV = ../../drivers/malikm

malikm-host: test.c sim.c mock.c $(DRV)/gpu.c $(DRV)/uapi.c $(DRV)/malikm.h mock/ntddk.h sim.h
	$(CC) $(CFLAGS) -o $@ test.c sim.c mock.c $(DRV)/gpu.c $(DRV)/uapi.c

run: malikm-host
	./malikm-host

clean:
	rm -f malikm-host
