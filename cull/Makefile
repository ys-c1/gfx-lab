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

# 增量视口更新测试（两段式差异 + 仅边界更新，逐项对齐全量）
test-inc: build/test_inc
	./build/test_inc

build/test_inc: $(SRC) test_inc.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test_inc.c -lm -o $@

# 增量 vs 全量基准：更新数/候选数/误差/耗时
bench: build/bench_inc
	./build/bench_inc

build/bench_inc: $(SRC) bench_inc.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) bench_inc.c -lm -o $@

demo: build/demo
	./build/demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

clean:
	rm -rf build demo.ppm demo.wav

.PHONY: all test test-inc bench demo clean
