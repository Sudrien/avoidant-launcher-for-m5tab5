# SPDX-License-Identifier: MIT
# main/limport_core.c on the host, against fixtures made by gen_fixture.py
# with the `cryptography` package (pip install cryptography).
#
#   make -C m5tab5/test/limport
#
# Two runs: a key longer than 16 characters (cut to 16), placed to
# straddle a 4096-byte read; and a short one (cycled).
CC ?= cc
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
          -fno-sanitize-recover=all -I../../main
LDFLAGS := -fsanitize=address,undefined

all: run

limporttest: limporttest.c ../../main/limport_core.c ../../main/limport_core.h
	$(CC) $(CFLAGS) -o $@ limporttest.c ../../main/limport_core.c $(LDFLAGS)

run: limporttest
	rm -rf fx1 fx2
	python3 gen_fixture.py fx1 "Zq3-launcher-key-ABCDEFGH-0123456789" 4090
	python3 gen_fixture.py fx2 "short-key" 12000
	ASAN_OPTIONS=detect_leaks=1 ./limporttest fx1
	ASAN_OPTIONS=detect_leaks=1 ./limporttest fx2
	rm -rf fx1 fx2

clean:
	rm -rf limporttest fx1 fx2
.PHONY: all run clean
