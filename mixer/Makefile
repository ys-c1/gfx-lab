CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Isrc
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

all: test

test: build/test
	./build/test

build/test: $(SRC) test.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c -lm -o $@

demo: build/demo
	./build/demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

bench: build/benchmark
	./build/benchmark
	python3 ref_limit.py build

build/benchmark: $(SRC) benchmark.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) benchmark.c -lm -o $@

clean:
	rm -rf build demo.ppm demo.wav

.PHONY: all test demo bench clean
