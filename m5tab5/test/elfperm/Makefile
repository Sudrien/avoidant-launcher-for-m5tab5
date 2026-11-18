# main/elfperm.c on the host.
#
#   make -C m5tab5/test/elfperm
#   ./elfpermtest some.app.elf ...      (a report for each)
#
# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
          -fno-sanitize-recover=all -I../../main
LDFLAGS := -fsanitize=address,undefined
SRC := ../../main/elfperm.c ../../main/elfsym.c

all: run

elfpermtest: elfpermtest.c $(SRC) ../../main/elfperm.h ../../main/elfsym.h
	$(CC) $(CFLAGS) -o $@ elfpermtest.c $(SRC) $(LDFLAGS)

run: elfpermtest
	./elfpermtest

clean:
	rm -f elfpermtest
.PHONY: all run clean
