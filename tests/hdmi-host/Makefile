# maliwddm's HDMI arithmetic (drivers/maliwddm/hdmi_calc.h) on the host. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror

hdmi-host: test.c ../../drivers/maliwddm/hdmi_calc.h
	$(CC) $(CFLAGS) -o $@ test.c

run: hdmi-host
	./hdmi-host

clean:
	rm -f hdmi-host
