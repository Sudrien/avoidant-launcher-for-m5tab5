# SPDX-License-Identifier: MIT
# main/ymodem.c on the host. `make` builds rx_host; ./run.sh tests it
# against lrzsz's sz (any sz that speaks YMODEM: SZ=/path/to/sz), and
# `node send.mjs` against web/ymodem.js, the web flasher's sender.
CFLAGS ?= -O2 -Wall -Wextra -Werror
rx_host: rx_host.c ../../main/ymodem.c ../../main/ymodem.h
	$(CC) $(CFLAGS) -I ../../main -o $@ rx_host.c ../../main/ymodem.c
clean:
	rm -rf rx_host out
.PHONY: clean
