/*
 * migrate.h — legacy-to-default syntax migration for the toke reference
 * compiler.
 *
 * Reads a token stream produced in PROFILE_LEGACY mode and emits
 * equivalent default (56-char, lowercase) syntax to a FILE stream.
 *
 * Story: 11.3.5
 */

#ifndef TKC_MIGRATE_H
#define TKC_MIGRATE_H

#include <stdio.h>
#include "lexer.h"

/*
 * tkc_migrate — Transform legacy syntax tokens to default syntax.
 *
 * Parameters:
 *   src    — original source text (tokens reference it by offset)
 *   slen   — length of src in bytes
 *   toks   — token array produced by lex() in PROFILE_LEGACY mode
 *   tc     — number of tokens (including TK_EOF)
 *   out    — output FILE stream (typically stdout)
 *
 * Returns:
 *    0 on success.
 *   -1 on error.
 *
 * Transformation rules (legacy -> default):
 *   1. Single-letter keywords M=, F=, T=, I= become m=, f=, t=, i=
 *   2. Uppercase-initial type identifiers become $-prefixed lowercase
 *      (e.g. Vec2 -> $vec2, I64 -> $i64)
 *   3. All other tokens and whitespace are emitted verbatim.
 */
int tkc_migrate(const char *src, int slen, const Token *toks, int tc,
                FILE *out);

#endif /* TKC_MIGRATE_H */
