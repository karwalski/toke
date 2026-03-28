CC      = cc
CFLAGS  = -std=c99 -Wall -Wextra -Wpedantic -Werror -g
SRCS    = src/lexer.c src/parser.c src/names.c src/types.c \
          src/arena.c src/ir.c src/llvm.c src/diag.c src/main.c \
          src/stdlib/str.c
OBJS    = $(SRCS:.c=.o)
BIN     = tkc

.PHONY: all clean lint conform conform-check build-all ci test-stdlib test-stdlib-process test-stdlib-env test-stdlib-crypto test-stdlib-time test-stdlib-test

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

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
	    test/stdlib/test_crypto.c src/stdlib/crypto.c src/stdlib/str.c \
	    -framework CoreFoundation -framework Security
	./test/stdlib/test_crypto

test-stdlib-time:
	$(CC) $(CFLAGS) -o test/stdlib/test_time \
	    test/stdlib/test_time.c src/stdlib/tk_time.c
	./test/stdlib/test_time

test-stdlib-test:
	$(CC) $(CFLAGS) -o test/stdlib/test_tktest \
	    test/stdlib/test_tktest.c src/stdlib/tk_test.c
	./test/stdlib/test_tktest

clean:
	rm -f $(OBJS) $(BIN) test/stdlib/test_str test/stdlib/test_db test/stdlib/test_process test/stdlib/test_env test/stdlib/test_crypto test/stdlib/test_time test/stdlib/test_tktest fuzz-lexer fuzz-parser

FUZZ_FLAGS = -fsanitize=address,undefined,fuzzer -g

fuzz-lexer: test/fuzz/fuzz_lexer.c src/lexer.o src/diag.o src/arena.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-lexer $^

fuzz-parser: test/fuzz/fuzz_parser.c src/lexer.o src/parser.o src/diag.o src/arena.o src/names.o src/types.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-parser $^

fuzz: fuzz-lexer fuzz-parser
	./fuzz-lexer -max_total_time=60 test/fuzz/corpus/
	./fuzz-parser -max_total_time=60 test/fuzz/corpus/

.PHONY: fuzz fuzz-lexer fuzz-parser
