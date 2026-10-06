/*
 * migrate.h — legacy-to-default syntax migration for the toke reference
 * compiler.
 *
 * Reads a token stream produced in PROFILE_LEGACY mode and emits
 * equivalent default (59-char, lowercase) syntax to a FILE stream.
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

/*
 * tkc_migrate_keep_comments — migrate `src`, keeping every comment (127.161).
 *
 * Runs the same pipeline as tkc_migrate() and puts each comment back on the
 * line it came from: own-line, leading and trailing comments where they
 * were, `//` and `#` rewritten as `(* *)`, and a comment with code on both
 * sides of it on one line moved to the end of that line with a note on
 * stderr naming `path` and the line.  Design and placement rules:
 * docs/architecture/migrate-comment-preservation.md.
 *
 * Returns 0 with the result in *outp (heap, NUL-terminated; the caller frees
 * it) and its length in *outlen; 1 when it declines because a comment could
 * not be placed safely, with the reason and line (0 = the whole file) in
 * *why and nothing allocated; -1 on an internal failure.
 */
typedef struct {
    int  line;
    char reason[256];
} TkcMigrateDecline;

int tkc_migrate_keep_comments(const char *src, int slen, const char *path,
                              char **outp, int *outlen, TkcMigrateDecline *why);

#endif /* TKC_MIGRATE_H */
