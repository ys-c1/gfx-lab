CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS ?= -Isrc

# Reference-counted atlas path is opt-in.  With the switch OFF (the default)
# src/atlas_ref.c is not compiled at all, so the build and every byte of demo
# output are exactly the original ones.
ATLAS_USE_REFCOUNT ?= 0
ifeq ($(ATLAS_USE_REFCOUNT),0)
SRC := $(filter-out src/atlas_ref.c,$(wildcard src/*.c))
else
SRC := $(wildcard src/*.c)
CPPFLAGS += -DATLAS_USE_REFCOUNT=$(ATLAS_USE_REFCOUNT)
endif
HDR := $(wildcard src/*.h)
REF_SRC := src/atlas_ref.c

all: test

test: build/test
	./build/test

build/test: $(SRC) test.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) test.c -lm -o $@

test-ref: build/test_ref
	./build/test_ref

build/test_ref: $(SRC) $(REF_SRC) test_ref.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DATLAS_USE_REFCOUNT=1 $(CFLAGS) $(SRC) $(REF_SRC) test_ref.c -lm -o $@

demo: build/demo
	./build/demo

build/demo: $(SRC) demo.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) demo.c -lm -o $@

demo-ref: build/demo_ref build/demo
	./build/demo_ref
	./build/demo > build/demo_legacy.txt
	@cmp build/demo_legacy.txt testdata/demo_legacy.txt \
		&& echo "legacy path: byte-identical to testdata/demo_legacy.txt"

build/demo_ref: $(SRC) $(REF_SRC) demo_ref.c $(HDR)
	@mkdir -p build
	$(CC) $(CPPFLAGS) -DATLAS_USE_REFCOUNT=1 $(CFLAGS) $(SRC) $(REF_SRC) demo_ref.c -lm -o $@

verify: test test-ref demo-ref

clean:
	rm -rf build demo.ppm demo.wav

.PHONY: all test test-ref demo demo-ref verify clean
