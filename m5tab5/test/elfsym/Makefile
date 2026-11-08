# main/elfsym.c on the host.
#
#   make -C m5tab5/test/elfsym
#
# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
          -fno-sanitize-recover=all -I../../main
LDFLAGS := -fsanitize=address,undefined

all: run

elfsymtest: elfsymtest.c ../../main/elfsym.c ../../main/elfsym.h
	$(CC) $(CFLAGS) -o $@ elfsymtest.c ../../main/elfsym.c $(LDFLAGS)

run: elfsymtest
	./elfsymtest

clean:
	rm -f elfsymtest
.PHONY: all run clean
