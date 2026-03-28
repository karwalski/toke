/*
 * fuzz_parser.c — libFuzzer entry point for the toke parser.
 *
 * Exercises the full lex → parse pipeline on arbitrary byte sequences.
 *
 * Build:  make fuzz-parser   (requires clang with -fsanitize=fuzzer)
 * Story:  1.7.2
 */

#include <stdint.h>
#include <stddef.h>

#include "../../src/parser.h"   /* Token, TokenKind, Node, parse() */
#include "../../src/arena.h"    /* Arena, arena_init, arena_free    */
#include "../../src/diag.h"     /* diag_reset                       */

/* Forward declaration — lexer.h is still a stub; real impl in lexer.c */
int lex(const char *src, int src_len, Token *tokens, int token_cap);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    diag_reset();

    /* Lex phase */
    int token_cap = (int)(size / 2) + 16;
    Token tokens[token_cap > 0 ? token_cap : 16];

    int count = lex((const char *)data, (int)size, tokens, token_cap);

    /* Parse phase — arena must be freed after every invocation to prevent
     * memory growth across fuzz runs (libFuzzer reuses the process). */
    Arena arena;
    arena_init(&arena);

    /* Ignore the returned root node; we are looking for crashes and
     * sanitizer findings, not semantic correctness. */
    parse(tokens, count, (const char *)data, &arena);

    arena_free(&arena);
    return 0;
}
