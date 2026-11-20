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

# 软边阴影（三段式：距离场半径 -> 可分离模糊 -> 合成/软边宽度）
soft-test: build/test_soft
	./build/test_soft

build/test_soft: $(SRC) test_soft.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test_soft.c -lm -o $@

soft-demo: build/demo_soft
	./build/demo_soft

build/demo_soft: $(SRC) demo_soft.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo_soft.c -lm -o $@

clean:
	rm -rf build demo.pgm demo.gpak soft_*.pgm kernel_*.txt

.PHONY: all test demo soft-test soft-demo clean
