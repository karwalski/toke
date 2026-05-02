/*
 * migrate.c — Source migration for the toke reference compiler.
 *
 * Implements --migrate: converts v0.2 (or partially migrated) toke source
 * to v0.3 default syntax. Handles both legacy (80-char) and partially
 * migrated (mixed old/new) input.
 *
 * Transformations:
 *   1. Legacy uppercase keywords (M=,F=,T=,I=) → lowercase (m=,f=,t=,i=)
 *   2. PascalCase type identifiers → $lowercase ($Vec2 → $vec2)
 *   3. |{ match syntax → mt expr { (v0.3 keyword-led match)
 *   4. Underscore removal: to_int → toint, from_bytes → frombytes
 *   5. $snake_case variants → $concatenated ($not_found → $notfound)
 *   6. Comment stripping: (* ... *) removed (comment-free design)
 *   7. Bitwise operators: ^ ~ << >> emit warnings (deferred to v0.5)
 *
 * Safe for partially migrated files: each transform is idempotent.
 *
 * Stories: 11.3.5, 82.3.1
 */

#include "migrate.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

/*
 * remove_underscores — Remove underscores from an identifier, writing
 *                      concatenated lowercase to out. Returns chars written.
 */
static int remove_underscores(const char *s, int len, char *out, int cap)
{
    int w = 0;
    for (int i = 0; i < len && w < cap - 1; i++) {
        if (s[i] != '_') out[w++] = s[i];
    }
    out[w] = '\0';
    return w;
}

/*
 * is_in_string — Check if byte offset `pos` is inside a string literal
 *                by scanning from the start. Simple: count unescaped quotes.
 */
static int is_in_string(const char *src, int pos)
{
    int in_str = 0;
    for (int i = 0; i < pos; i++) {
        if (src[i] == '"' && (i == 0 || src[i-1] != '\\')) in_str = !in_str;
    }
    return in_str;
}

/* ── Public API ───────────────────────────────────────────────────── */

int tkc_migrate(const char *src, int slen, const Token *toks, int tc,
                FILE *out)
{
    /*
     * Phase 1: Token-level migration into a buffer.
     * Handles: uppercase keywords, PascalCase types, underscore removal,
     * $snake_case → $concatenated, comment stripping, bitwise warnings.
     */
    char *buf = malloc((size_t)(slen * 2 + 1024));
    if (!buf) return -1;
    int blen = 0;

    int prev_end = 0;

    for (int i = 0; i < tc; i++) {
        const Token *t = &toks[i];
        if (t->kind == TK_EOF) break;

        /* Copy gap (whitespace between tokens) but strip comments */
        for (int g = prev_end; g < t->start; g++) {
            /* Detect (* ... *) comments and skip them */
            if (g + 1 < t->start && src[g] == '(' && src[g+1] == '*') {
                int depth = 1;
                g += 2;
                while (g < slen && depth > 0) {
                    if (g + 1 < slen && src[g] == '(' && src[g+1] == '*') { depth++; g += 2; continue; }
                    if (g + 1 < slen && src[g] == '*' && src[g+1] == ')') { depth--; g += 2; continue; }
                    g++;
                }
                g--; /* for loop will increment */
                continue;
            }
            buf[blen++] = src[g];
        }

        switch (t->kind) {
        case TK_KW_M: case TK_KW_F: case TK_KW_T: case TK_KW_I:
            /* Uppercase single-letter → lowercase */
            if (t->len == 1 && src[t->start] >= 'A' && src[t->start] <= 'Z') {
                buf[blen++] = (char)(src[t->start] + ('a' - 'A'));
            } else {
                memcpy(buf + blen, src + t->start, (size_t)t->len);
                blen += t->len;
            }
            break;

        case TK_TYPE_IDENT:
            /* PascalCase → $lowercase */
            buf[blen++] = '$';
            for (int j = 0; j < t->len; j++) {
                char c = src[t->start + j];
                buf[blen++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
            }
            break;

        case TK_IDENT: {
            /* Remove underscores from identifiers */
            char clean[256];
            int clen = remove_underscores(src + t->start, t->len, clean, sizeof clean);
            memcpy(buf + blen, clean, (size_t)clen);
            blen += clen;
            break;
        }

        case TK_DOLLAR: {
            /* $variant_name → $variantname (strip underscores after $) */
            buf[blen++] = '$';
            /* The $ token is just the dollar sign; the following IDENT
             * will be processed in the TK_IDENT case above. */
            break;
        }

        case TK_CARET: case TK_TILDE:
            /* Bitwise operators deferred — emit warning comment */
            fprintf(stderr, "migrate: warning: bitwise operator '%.*s' at line %d "
                    "deferred to v0.5, removing\n", t->len, src + t->start, t->line);
            break;

        case TK_SHL: case TK_SHR:
            fprintf(stderr, "migrate: warning: shift operator '%.*s' at line %d "
                    "deferred to v0.5, removing\n", t->len, src + t->start, t->line);
            break;

        default:
            memcpy(buf + blen, src + t->start, (size_t)t->len);
            blen += t->len;
            break;
        }

        prev_end = t->start + t->len;
    }

    /* Trailing content */
    for (int g = prev_end; g < slen; g++) buf[blen++] = src[g];
    buf[blen] = '\0';

    /*
     * Phase 2: Text-level migration.
     * Handles: |{ match → mt expr {
     * This must be text-level because the pipe-brace pattern spans
     * two tokens and the expression before it becomes the scrutinee.
     *
     * Pattern: `expr|{` → `mt expr {`
     * We search for `|{` NOT inside string literals.
     */
    char *buf2 = malloc((size_t)(blen * 2 + 256));
    if (!buf2) { free(buf); return -1; }
    int b2len = 0;

    for (int p = 0; p < blen; p++) {
        if (buf[p] == '|' && p + 1 < blen && buf[p+1] == '{' && !is_in_string(buf, p)) {
            /* Found |{ — need to insert `mt ` before the scrutinee expression.
             * Walk backwards to find where the expression starts.
             * Simple heuristic: the expression starts after the last `;` or `{`
             * or `=` or at the start of line. */
            int expr_start = p;
            while (expr_start > 0) {
                char pc = buf[expr_start - 1];
                if (pc == ';' || pc == '{' || pc == '=' || pc == '\n') break;
                expr_start--;
            }
            /* Skip leading whitespace in the expression */
            while (expr_start < p && (buf[expr_start] == ' ' || buf[expr_start] == '\t'))
                expr_start++;

            /* Rewind b2len to expr_start — we need to re-emit with `mt ` prefix */
            /* Calculate how many chars of the expression we already wrote */
            int already_written = p - expr_start;
            b2len -= already_written;

            /* Emit: mt <expr> { */
            memcpy(buf2 + b2len, "mt ", 3);
            b2len += 3;
            memcpy(buf2 + b2len, buf + expr_start, (size_t)(p - expr_start));
            b2len += (p - expr_start);
            buf2[b2len++] = ' ';
            buf2[b2len++] = '{';
            p++; /* skip the { after | */
        } else {
            buf2[b2len++] = buf[p];
        }
    }
    buf2[b2len] = '\0';

    /* Write result */
    fwrite(buf2, 1, (size_t)b2len, out);
    free(buf);
    free(buf2);
    return 0;
}
