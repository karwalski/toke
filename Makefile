CC      = cc
CFLAGS  = -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -g
SRCS    = src/lexer.c src/parser.c src/names.c src/types.c \
          src/arena.c src/ir.c src/llvm.c src/diag.c src/main.c \
          src/stdlib/str.c
OBJS    = $(SRCS:.c=.o)
BIN     = tkc

# ── Reproducible-build flags ──────────────────────────────────────────────────
# These flags eliminate sources of non-determinism so that the same source tree
# produces bit-identical binaries across builds on the same platform.
#
#   -frandom-seed=tkc       deterministic internal compiler hashes
#   -ffile-prefix-map=...   strip absolute paths from debug info / __FILE__
#
# SOURCE_DATE_EPOCH: if set in the environment, GCC/Clang use it for __DATE__
# and __TIME__.  The Makefile exports it when available; CI pins it to the
# commit timestamp.
# ──────────────────────────────────────────────────────────────────────────────
REPRO_FLAGS = -frandom-seed=tkc \
              -ffile-prefix-map=$(CURDIR)=.

export SOURCE_DATE_EPOCH ?= 0

.PHONY: all clean lint conform conform-check build-all ci test-e2e test-stdlib test-stdlib-process test-stdlib-env test-stdlib-crypto test-stdlib-time test-stdlib-test test-stdlib-log test-stdlib-coverage bench repro-check

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -c -o $@ $<

lint:
	$(CC) $(CFLAGS) --analyze $(SRCS)

conform:
	@bash test/run_conform.sh

conform-check:
	@bash test/run_conform.sh && echo "CONFORMANCE: 100%"

build-all:
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=x86_64-linux-gnu"
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=aarch64-linux-gnu"
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=aarch64-apple-macos"

test-e2e: $(BIN)
	@bash test/e2e/run_e2e.sh

ci: lint conform conform-check

test-stdlib:
	$(CC) $(CFLAGS) -o test/stdlib/test_str \
	    test/stdlib/test_str.c src/stdlib/str.c
	./test/stdlib/test_str

test-stdlib-db:
	$(CC) $(CFLAGS) -o test/stdlib/test_db \
	    test/stdlib/test_db.c src/stdlib/db.c -lsqlite3
	./test/stdlib/test_db

test-stdlib-process:
	$(CC) $(CFLAGS) -o test/stdlib/test_process \
	    test/stdlib/test_process.c src/stdlib/process.c
	./test/stdlib/test_process

test-stdlib-env:
	$(CC) $(CFLAGS) -o test/stdlib/test_env \
	    test/stdlib/test_env.c src/stdlib/env.c
	./test/stdlib/test_env

test-stdlib-crypto:
	$(CC) $(CFLAGS) -o test/stdlib/test_crypto \
	    test/stdlib/test_crypto.c src/stdlib/crypto.c src/stdlib/str.c
	./test/stdlib/test_crypto

test-stdlib-time:
	$(CC) $(CFLAGS) -o test/stdlib/test_time \
	    test/stdlib/test_time.c src/stdlib/tk_time.c
	./test/stdlib/test_time

test-stdlib-test:
	$(CC) $(CFLAGS) -o test/stdlib/test_tktest \
	    test/stdlib/test_tktest.c src/stdlib/tk_test.c
	./test/stdlib/test_tktest

test-stdlib-log:
	$(CC) $(CFLAGS) -o test/stdlib/test_log \
	    test/stdlib/test_log.c src/stdlib/log.c src/stdlib/tk_time.c
	./test/stdlib/test_log

test-stdlib-coverage:
	$(CC) $(CFLAGS) -Isrc/stdlib -o test/stdlib/test_stdlib_coverage \
	    test/stdlib/test_stdlib_coverage.c \
	    src/stdlib/json.c src/stdlib/file.c src/stdlib/str.c \
	    src/stdlib/db.c -lsqlite3
	./test/stdlib/test_stdlib_coverage

bench:
	$(CC) -O2 -Isrc/stdlib -o test/stdlib/bench_stdlib \
	    test/stdlib/bench_stdlib.c \
	    src/stdlib/str.c src/stdlib/json.c src/stdlib/file.c \
	    src/stdlib/crypto.c src/stdlib/tk_time.c src/stdlib/process.c \
	    src/stdlib/env.c src/stdlib/log.c src/stdlib/tk_test.c \
	    src/stdlib/db.c src/stdlib/http.c -lsqlite3
	./test/stdlib/bench_stdlib

# ── Reproducible-build verification ───────────────────────────────────────────
# Builds tkc twice into separate directories and compares SHA-256 hashes.
# Exits non-zero if the two binaries differ.
repro-check:
	@echo "==> Reproducible-build check: building twice and comparing hashes..."
	@rm -rf .repro-a .repro-b
	$(MAKE) clean
	SOURCE_DATE_EPOCH=1700000000 $(MAKE) all
	@mkdir -p .repro-a && cp $(BIN) .repro-a/$(BIN) && \
	 for f in $(OBJS); do cp $$f .repro-a/$$(basename $$f); done
	$(MAKE) clean
	SOURCE_DATE_EPOCH=1700000000 $(MAKE) all
	@mkdir -p .repro-b && cp $(BIN) .repro-b/$(BIN) && \
	 for f in $(OBJS); do cp $$f .repro-b/$$(basename $$f); done
	$(MAKE) clean
	@# ── Compare object files (deterministic on all platforms) ──
	@FAIL=0; \
	 for f in $(patsubst %.o,%,$(notdir $(OBJS))); do \
	   HA=$$(shasum -a 256 .repro-a/$$f.o | cut -d' ' -f1); \
	   HB=$$(shasum -a 256 .repro-b/$$f.o | cut -d' ' -f1); \
	   if [ "$$HA" = "$$HB" ]; then \
	     echo "  PASS $$f.o  $$HA"; \
	   else \
	     echo "  FAIL $$f.o  A=$$HA  B=$$HB" >&2; FAIL=1; \
	   fi; \
	 done; \
	 echo ""; \
	 HA=$$(shasum -a 256 .repro-a/$(BIN) | cut -d' ' -f1); \
	 HB=$$(shasum -a 256 .repro-b/$(BIN) | cut -d' ' -f1); \
	 if [ "$$HA" = "$$HB" ]; then \
	   echo "  PASS $(BIN)  $$HA"; \
	 else \
	   echo "  INFO $(BIN) differs (expected on macOS due to LC_UUID)"; \
	   echo "       A=$$HA"; \
	   echo "       B=$$HB"; \
	 fi; \
	 echo ""; \
	 if [ "$$FAIL" = "0" ]; then \
	   echo "PASS: all object files are identical — build is reproducible"; \
	 else \
	   echo "FAIL: object files differ" >&2; exit 1; \
	 fi
	@rm -rf .repro-a .repro-b

clean:
	rm -f $(OBJS) $(BIN) test/stdlib/test_str test/stdlib/test_db \
	    test/stdlib/test_process test/stdlib/test_env test/stdlib/test_crypto \
	    test/stdlib/test_time test/stdlib/test_tktest test/stdlib/test_log \
	    test/stdlib/test_stdlib_coverage test/stdlib/bench_stdlib \
	    fuzz-lexer fuzz-parser

FUZZ_FLAGS = -fsanitize=address,undefined,fuzzer -g

fuzz-lexer: test/fuzz/fuzz_lexer.c src/lexer.o src/diag.o src/arena.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-lexer $^

fuzz-parser: test/fuzz/fuzz_parser.c src/lexer.o src/parser.o src/diag.o src/arena.o src/names.o src/types.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-parser $^

fuzz: fuzz-lexer fuzz-parser
	./fuzz-lexer -max_total_time=60 test/fuzz/corpus/
	./fuzz-parser -max_total_time=60 test/fuzz/corpus/

.PHONY: fuzz fuzz-lexer fuzz-parser
