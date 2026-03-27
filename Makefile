CC      = cc
CFLAGS  = -std=c99 -Wall -Wextra -Wpedantic -Werror -g
SRCS    = src/lexer.c src/parser.c src/names.c src/types.c \
          src/arena.c src/ir.c src/llvm.c src/diag.c src/main.c
OBJS    = $(SRCS:.c=.o)
BIN     = tkc

.PHONY: all clean lint conform conform-check build-all ci

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

lint:
	$(CC) $(CFLAGS) --analyze $(SRCS)

conform:
	@echo "Conformance suite not yet implemented"
	@exit 1

conform-check:
	@echo "Conformance check not yet implemented"
	@exit 1

build-all:
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=x86_64-linux-gnu"
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=aarch64-linux-gnu"
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=aarch64-apple-macos"

ci: lint conform conform-check

clean:
	rm -f $(OBJS) $(BIN)
