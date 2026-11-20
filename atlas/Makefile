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

sanitize: build/test-san
	./build/test-san

build/test-san: $(SRC) test.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -O1 -g -Wall -Wextra -std=c11 \
		-fsanitize=address,undefined $(SRC) test.c -lm -o $@

demo: build/demo
	./build/demo

bench: demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

clean:
	rm -rf build demo.ppm demo.wav

.PHONY: all test sanitize demo bench clean
