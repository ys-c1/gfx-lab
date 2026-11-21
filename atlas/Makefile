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

# 超限分册（默认关闭）：单独编译，老路径的 test/demo 不受影响。
VOLFLAGS := -DATLAS_USE_VOL=1

test-vol: build/test_vol
	./build/test_vol

build/test_vol: src/atlas.c src/atlas_vol.c src/atlas_vol.h src/atlas.h \
                test_vol.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VOLFLAGS) src/atlas.c src/atlas_vol.c \
	      test_vol.c -lm -o $@

demo-vol: build/demo_vol
	./build/demo_vol

build/demo_vol: src/atlas.c src/atlas_vol.c src/atlas_vol.h src/atlas.h \
                demo_vol.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(VOLFLAGS) src/atlas.c src/atlas_vol.c \
	      demo_vol.c -lm -o $@

verify: test test-vol demo-vol

clean:
	rm -rf build demo.ppm demo.wav demo_vol_1x.txt demo_vol_2x.txt

.PHONY: all test demo test-vol demo-vol verify clean
