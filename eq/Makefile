CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Isrc
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
PYTHON ?= python3

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

# 独立脚本：用纯 stdlib 手算系数与幅频响应对照导出的 demo_coef.csv
verify: demo
	$(PYTHON) -I verify/handcalc.py

clean:
	rm -rf build demo_eq.wav demo_coef.csv

.PHONY: all test demo verify clean
