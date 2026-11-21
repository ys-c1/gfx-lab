CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Isrc
PYTHON ?= python3
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

# 等功率声像：两段式（增益 -> 分配）。旧 test/demo 目标保持不变。
pan-test: build/test_pan
	./build/test_pan

build/test_pan: $(SRC) test.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c -lm -o $@

pan-demo: build/pan_demo
	./build/pan_demo

build/pan_demo: $(SRC) pan_demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) pan_demo.c -lm -o $@

pan-bench: build/pan_bench
	./build/pan_bench
	$(PYTHON) -I pan/verify/handcalc.py build

build/pan_bench: $(SRC) pan_bench.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) pan_bench.c -lm -o $@

clean:
	rm -rf build demo.ppm demo.wav demo_pan_*.wav

.PHONY: all test demo pan-test pan-demo pan-bench clean
