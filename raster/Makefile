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

nearcheck: build/nearcheck
	./build/nearcheck

build/nearcheck: $(SRC) nearcheck.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) nearcheck.c -lm -o $@

handcheck:
	python3 -I handcheck.py

clean:
	rm -rf build demo.ppm demo32.ppm demo256.ppm off.ppm on.ppm

.PHONY: all test demo nearcheck handcheck clean
