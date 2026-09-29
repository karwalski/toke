/*
 * fuzz_parser.c — libFuzzer entry point for the toke parser.
 *
 * Exercises the full lex → parse pipeline on arbitrary byte sequences, the
 * way tkc drives it: parse only a token stream the lexer produced without
 * error (src/main.c does the same).
 *
 * Build:  make fuzz-parser   (FUZZ_ENGINE=libfuzzer|standalone|auto)
 * Story:  1.7.2; 121.30 (brought back to the arena API)
 *
 * 121.30: this file no longer built -- it declared `Arena arena` on the
 * stack and called arena_init(&arena), but Arena has been an opaque type
 * returned by arena_init() for a long time ("variable has incomplete type
 * 'Arena'").  The nightly fuzz job was red at the compile step.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../../src/lexer.h"    /* Token, Profile, lex()   */
#include "../../src/parser.h"   /* parse()                 */
#include "../../src/arena.h"    /* arena_init, arena_free  */
#include "../../src/diag.h"     /* diag_reset, error count */

/* Diagnostics are counted as usual but not printed: a fuzzer writing a JSON
 * record to stderr for every input spends its time in I/O. */
static void quiet(DiagSeverity sev, int code, int off, int line, int col,
                  const char *msg, const char *fix, void *ctx)
{
    (void)sev; (void)code; (void)off; (void)line; (void)col;
    (void)msg; (void)fix; (void)ctx;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20)) return 0;
    diag_set_sink(quiet, NULL);
    diag_reset();
    int cap = (int)size + 16;
    Token *tokens = malloc((size_t)cap * sizeof(Token));
    if (!tokens) return 0;
    int count = lex((const char *)data, (int)size, tokens, cap, PROFILE_DEFAULT);
    if (count >= 0 && diag_error_count() == 0) {
        /* The arena is freed after every input: libFuzzer reuses the
         * process, and a leak here would read as a finding. */
        Arena *arena = arena_init();
        if (arena) {
            parse(tokens, count, (const char *)data, arena, PROFILE_DEFAULT);
            arena_free(arena);
        }
    }
    free(tokens);
    return 0;
}
