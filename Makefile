# styled-qr-c: build the preview tool, render the examples, check them.
#
#   make            build build/preview
#   make examples   render assets/examples and print the check table
#
# There is nothing to build for the library itself -- it is two translation
# units you add to your own project. This target exists to run the checker.

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
INCLUDE  = -Isrc -Ithird_party/qrcodegen -Iexamples
SRCS     = third_party/qrcodegen/qrcodegen.c src/qrstyle.c examples/preview.c
OUT      = build/preview
TEXT    ?= https://github.com/MRobertEvers/styled-qr-c

.PHONY: all examples clean

all: $(OUT)

$(OUT): $(SRCS) src/qrstyle.h third_party/qrcodegen/qrcodegen.h examples/logo_tori.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRCS)

# Rewrites assets/examples in place and exits non-zero if any example fails
# the scanner-side check.
examples: $(OUT)
	./$(OUT) assets/examples "$(TEXT)"

clean:
	rm -rf build
