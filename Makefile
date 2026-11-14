# styled-qr-c: build the preview tool, render the examples, check them.
#
#   make            build build/preview
#   make examples   render assets/examples and print the check table
#   make web        rebuild docs/qrstyle.wasm for the GitHub Pages demo (emcc)
#
# There is nothing to build for the library itself -- it is two translation
# units you add to your own project. This target exists to run the checker.

CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra
INCLUDE  = -Isrc -Iexamples
SRCS     = src/qrcodegen.c src/qrstyle.c examples/preview.c
OUT      = build/preview
TEXT    ?= https://github.com/MRobertEvers/styled-qr-c

.PHONY: all examples web clean

all: $(OUT)

$(OUT): $(SRCS) src/qrstyle.h src/qrcodegen.h examples/logo_tori.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $(SRCS)

# Rewrites assets/examples in place and exits non-zero if any example fails
# the scanner-side check.
examples: $(OUT)
	./$(OUT) assets/examples "$(TEXT)"

# The browser demo in docs/: the same two translation units, compiled to a
# standalone wasm module with no imports. docs/app.js is the page around it.
EMCC    ?= emcc
WASM     = docs/qrstyle.wasm

web: $(WASM)

$(WASM): src/qrcodegen.c src/qrstyle.c web/qrstyle_web.c src/qrstyle.h src/qrcodegen.h examples/logo_tori.h
	$(EMCC) -O2 -DNDEBUG -std=c99 -Wall -Wextra $(INCLUDE) \
		src/qrcodegen.c src/qrstyle.c web/qrstyle_web.c -o $@ \
		--no-entry -sSTANDALONE_WASM -sINITIAL_MEMORY=16MB \
		-sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=256KB

clean:
	rm -rf build
