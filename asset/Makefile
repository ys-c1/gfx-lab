CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
LDLIBS ?= -lz -lm
CPPFLAGS ?= -Isrc
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

all: test

test: build/test
	./build/test

build/test: $(SRC) test.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c $(LDLIBS) -o $@

bench: build/bench
	./build/bench

build/bench: $(SRC) bench.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) bench.c $(LDLIBS) -o $@

demo: build/demo
	./build/demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c $(LDLIBS) -o $@

clean:
	rm -rf build demo.pgm demo.gpak

.PHONY: all test bench demo clean
