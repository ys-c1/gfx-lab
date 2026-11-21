CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -pipe
CPPFLAGS ?= -Isrc
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

all: test

test: build/test build/test_poly
	./build/test
	./build/test_poly

build/test: $(SRC) test.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c -lm -o $@

build/test_poly: $(SRC) test_poly.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test_poly.c -lm -o $@

bench: build/bench_poly
	@mkdir -p bench_out
	./build/bench_poly bench_out

build/bench_poly: $(SRC) bench_poly.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) bench_poly.c -lm -o $@

demo: build/demo
	./build/demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

clean:
	rm -rf build demo.ppm demo.wav bench_out

.PHONY: all test bench demo clean
