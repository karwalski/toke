/*
 * migrate.c — Legacy-to-default syntax migration for the toke reference
 *             compiler.
 *
 * =========================================================================
 * Role
 * =========================================================================
 * This module implements the --migrate flag.  It walks a token stream
 * produced by lexing in PROFILE_LEGACY mode and emits equivalent default
 * (56-char, lowercase) syntax.
 *
 * The transformation is purely token-level: whitespace and comments between
 * tokens are preserved verbatim by copying the source text gaps between
 * consecutive token spans.
 *
 * Story: 11.3.5
 */

#include "migrate.h"

/* ── Helpers ──────────────────────────────────────────────────────── */

/*
 * emit_gap — Write the source text between the end of the previous token
 *            and the start of the current token.  This preserves whitespace,
 *            comments, and any other inter-token characters verbatim.
 */
static void emit_gap(const char *src, int gap_start, int gap_end, FILE *out)
{
    for (int i = gap_start; i < gap_end; i++) {
        fputc(src[i], out);
    }
}

/*
 * emit_lowercase — Write `len` bytes from src+start, lowercasing ASCII
 *                  uppercase letters.
 */
static void emit_lowercase(const char *src, int start, int len, FILE *out)
{
    for (int i = 0; i < len; i++) {
        char c = src[start + i];
        if (c >= 'A' && c <= 'Z') {
            fputc(c + ('a' - 'A'), out);
        } else {
            fputc(c, out);
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

int tkc_migrate(const char *src, int slen, const Token *toks, int tc,
                FILE *out)
{
    int prev_end = 0;  /* byte offset just past the previous token */

    for (int i = 0; i < tc; i++) {
        const Token *t = &toks[i];

        /* Skip the EOF sentinel — it has zero length. */
        if (t->kind == TK_EOF) break;

        /* Emit any whitespace / gap between previous token and this one. */
        emit_gap(src, prev_end, t->start, out);

        switch (t->kind) {
        case TK_KW_M:
        case TK_KW_F:
        case TK_KW_T:
        case TK_KW_I:
            /*
             * Single-letter keywords: if the lexeme is a single uppercase
             * letter (M, F, T, I), emit it as lowercase.  Multi-char
             * keywords that happen to share the enum (shouldn't occur for
             * these four) are emitted verbatim.
             */
            if (t->len == 1 && src[t->start] >= 'A' && src[t->start] <= 'Z') {
                fputc(src[t->start] + ('a' - 'A'), out);
            } else {
                fwrite(src + t->start, 1, (size_t)t->len, out);
            }
            break;

        case TK_TYPE_IDENT:
            /*
             * Uppercase-initial type identifiers become $-prefixed
             * lowercase in default syntax.
             * e.g. Vec2 -> $vec2, I64 -> $i64, Str -> $str
             */
            fputc('$', out);
            emit_lowercase(src, t->start, t->len, out);
            break;

        default:
            /* All other tokens: emit verbatim from source. */
            fwrite(src + t->start, 1, (size_t)t->len, out);
            break;
        }

        prev_end = t->start + t->len;
    }

    /* Emit any trailing content after the last token (trailing newline, etc.) */
    if (prev_end < slen) {
        emit_gap(src, prev_end, slen, out);
    }

    return 0;
}
