CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Isrc
SRC := $(wildcard src/*.c)
HDR := $(wildcard src/*.h)

all: test

test: build/test
	./build/test

build/test: $(SRC) test.c $(HDR)
	@mkdir -p build/t
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c -lm -o $@

demo: build/demo
	./build/demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

bench: build/bench
	./build/bench

build/bench: $(SRC) bench.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) bench.c -lm -o $@

verify: build/bench
	./build/bench
	python3 -I tools/hand_parse.py verify build/bench_v1.gpak build/bench_v2.gpak
	python3 -I tools/hand_parse.py parse build/bench_v1.gpak --migrate
	python3 -I tools/hand_parse.py parse build/bench_v2.gpak

clean:
	rm -rf build demo.pgm demo.gpak

.PHONY: all test demo bench verify clean
