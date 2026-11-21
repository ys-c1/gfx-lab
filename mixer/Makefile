CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Isrc
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)
OUT := ../artifacts/softclip

all: test

test: build/test
	./build/test

build/test: $(SRC) test.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c -lm -o $@

demo: build/demo
	@mkdir -p $(OUT)
	./build/demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

# Python 独立解析解复核 + SVG 曲线出图 (不依赖第三方库)
verify: demo
	python3 -I tools/verify.py $(OUT)

clean:
	rm -rf build demo.ppm demo.wav

.PHONY: all test demo verify clean
