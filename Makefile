CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L

all: bserve bcurl

bserve: src/bserve.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bserve.c src/proto.c

bcurl: src/bcurl.c src/proto.c src/proto.h
	$(CC) $(CFLAGS) -o $@ src/bcurl.c src/proto.c

test: all
	python3 tests/interop_test.py

clean:
	rm -f bserve bcurl

.PHONY: all test clean
