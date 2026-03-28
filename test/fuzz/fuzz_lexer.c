/*
 * fuzz_lexer.c — libFuzzer entry point for the toke lexer.
 *
 * Build:  make fuzz-lexer   (requires clang with -fsanitize=fuzzer)
 * Story:  1.7.2
 */

#include <stdint.h>
#include <stddef.h>

#include "../../src/parser.h"   /* Token, TokenKind */
#include "../../src/diag.h"     /* diag_reset        */

/* Forward declaration — lexer.h is still a stub; real impl in lexer.c */
int lex(const char *src, int src_len, Token *tokens, int token_cap);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    diag_reset();

    /*
     * Upper-bound on token count: no input can produce more tokens than
     * bytes, plus a small margin for the mandatory TK_EOF sentinel.
     * Allocate on the stack — keeps the fuzz target allocation-free.
     */
    int token_cap = (int)(size / 2) + 16;
    Token tokens[token_cap > 0 ? token_cap : 16];

    /* Drive the lexer; ignore the returned token count.
     * A crash or sanitizer finding here is a P0 bug. */
    lex((const char *)data, (int)size, tokens, token_cap);

    return 0;
}
