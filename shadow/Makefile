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

# Curve-weighted penumbra: two-stage distance->weight->blend pipeline.
pen-test: build/test_pen
	./build/test_pen

build/test_pen: $(SRC) test_pen.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test_pen.c -lm -o $@

pen-demo: build/demo_pen
	./build/demo_pen

build/demo_pen: $(SRC) demo_pen.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo_pen.c -lm -o $@

pen-bench: build/bench_pen
	./build/bench_pen

build/bench_pen: $(SRC) bench_pen.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) bench_pen.c -lm -o $@

clean:
	rm -rf build demo.pgm demo_soft.pgm demo_curve.tsv demo.gpak

.PHONY: all test demo pen-test pen-demo pen-bench clean
