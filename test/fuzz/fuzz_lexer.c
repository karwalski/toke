/*
 * fuzz_lexer.c — libFuzzer entry point for the toke lexer.
 *
 * Build:  make fuzz-lexer   (FUZZ_ENGINE=libfuzzer|standalone|auto)
 * Story:  1.7.2; 121.30 (brought back to the lexer's real signature)
 *
 * 121.30: this file had drifted from the compiler it tests and no longer
 * built.  It declared its own four-argument lex() while lexer.h's takes a
 * Profile as well -- "conflicting types for 'lex'" -- so the nightly fuzz
 * job that built it (nightly.yml) was red at the compile step, and nothing
 * else built it.  It now includes lexer.h and sizes the token buffer the way
 * tkc does (src_len + 16), on the heap rather than a VLA a large input would
 * blow the stack with.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../../src/lexer.h"    /* Token, Profile, lex() */
#include "../../src/diag.h"     /* diag_reset             */

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
    /* A crash or sanitizer finding here is a P0 bug. */
    lex((const char *)data, (int)size, tokens, cap, PROFILE_DEFAULT);
    lex((const char *)data, (int)size, tokens, cap, PROFILE_LEGACY);
    free(tokens);
    return 0;
}
