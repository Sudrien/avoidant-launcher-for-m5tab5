# main/boxns.c on the host (u-avoidant-0004).
#
#   make -C m5tab5/test/boxns
#
# SPDX-License-Identifier: MIT
CC ?= cc
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
          -fno-sanitize-recover=all -I../../main
LDFLAGS := -fsanitize=address,undefined

all: run

boxnstest: boxnstest.c ../../main/boxns.c ../../main/boxns.h
	$(CC) $(CFLAGS) -o $@ boxnstest.c ../../main/boxns.c $(LDFLAGS)

run: boxnstest
	./boxnstest

clean:
	rm -f boxnstest
.PHONY: all run clean
