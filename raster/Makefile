CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Isrc
# 本机 /tmp 可能写满；编译器临时文件落到模块内的 .tmp（已在 .gitignore）
TMPDIR := $(CURDIR)/.tmp
export TMPDIR
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

all: test

test: build/test
	./build/test

build/test: $(SRC) test.c $(HDR)
	@mkdir -p build .tmp
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c -lm -o $@

demo: build/demo
	./build/demo

refcheck:
	python3 -I tools/refcheck.py 32
	python3 -I tools/refcheck.py 256
	python3 -I tools/refcheck.py 256 --ppm build/ref256.ppm
	cmp demo.ppm build/ref256.ppm && echo "PPM byte-identical: C demo vs python ref"

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build .tmp
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

clean:
	rm -rf build .tmp demo.ppm demo.wav

.PHONY: all test demo refcheck clean
