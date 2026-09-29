/*
 * regex_check.c — E4075: a literal regex pattern outside the supported
 * language is rejected at compile time (story 137.9).
 *
 * Owner decision 2026-09-22: reject unsupported regex syntax at compile time.
 * Before this, `str.replacere(s;"\\s";"_")` compiled, ran, and returned s
 * unchanged, because regcomp's failure left the result at "no match".
 *
 * Runs after type checking, as its own pass.  It walks the AST for calls
 * `A.fn(subject; "pattern" ...)` where A is an alias of std.str and fn is a
 * regex entry point, decodes the literal exactly as the lexer's escapes
 * define it, and compiles it with tk_regex.h -- the runtime's own engine, so
 * the compiler and the program can never disagree about what is valid.
 */
#include "regex_check.h"
#include "diag.h"
#include "stdlib/tk_regex.h"

#include <stdlib.h>
#include <string.h>

static const char *const REGEX_FNS[] = {
    "containsre", "countre", "findall", "matches", "replacere", NULL
};

static int ident_is(const Node *n, const char *src, const char *s) {
    size_t l = strlen(s);
    return n && n->kind == NODE_IDENT && n->tok_len == (int)l &&
           memcmp(src + n->tok_start, s, l) == 0;
}

static int is_str_alias(const Node *n, const char *src, const SymbolTable *st) {
    if (!n || n->kind != NODE_IDENT) return 0;
    for (int i = 0; i < st->count; i++) {
        const ImportEntry *e = &st->entries[i];
        if (e->module_path && !strcmp(e->module_path, "std.str") &&
            e->alias_name && ident_is(n, src, e->alias_name))
            return 1;
    }
    return 0;
}

static int hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decodes the literal whose opening quote is at src[q] (raw bytes up to the
 * closing quote at src[end]) into out[], recording the source offset of
 * each decoded byte in at[].  Stops at a decoded NUL, as the runtime's C
 * string does.  Returns the decoded length, or -1 when the literal is not a
 * plain constant (it interpolates) and so cannot be checked here.
 */
static int decode(const char *src, int q, int end, char *out, int *at) {
    int n = 0;
    for (int i = q + 1; i < end; ) {
        int c = (unsigned char)src[i], start = i, v;
        if (c != '\\') { v = c; i++; }
        else {
            int e = (i + 1 < end) ? (unsigned char)src[i + 1] : 0;
            i += 2;
            switch (e) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case 'r': v = '\r'; break;
            case '0': v = 0; break;
            case '"': v = '"'; break;
            case '\\': v = '\\'; break;
            case 'x':
                if (i + 1 < end && hexv(src[i]) >= 0 && hexv(src[i + 1]) >= 0) {
                    v = hexv(src[i]) * 16 + hexv(src[i + 1]); i += 2;
                } else return -1;
                break;
            default: return -1;   /* \( interpolation, or an escape the lexer rejected */
            }
        }
        if (v == 0) break;
        at[n] = start; out[n++] = (char)v;
    }
    out[n] = '\0';
    at[n] = end;               /* an error "at the end" points at the closing quote */
    return n;
}

static void check_call(const Node *call, const char *src, int src_len, const SymbolTable *st, int *count) {
    if (call->child_count < 3) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2) return;
    if (!is_str_alias(callee->children[0], src, st)) return;
    const char *fn = NULL;
    for (int i = 0; REGEX_FNS[i]; i++)
        if (ident_is(callee->children[1], src, REGEX_FNS[i])) { fn = REGEX_FNS[i]; break; }
    if (!fn) return;
    const Node *lit = call->children[2];            /* (subject; pattern ...) */
    if (!lit || lit->kind != NODE_STR_LIT) return;
    int q = lit->tok_start, end = lit->tok_start + lit->tok_len - 1;
    /* the node must span a "..." in this source buffer (lowered nodes may not) */
    if (q < 0 || end <= q || end >= src_len || src[q] != '"' || src[end] != '"') return;
    int cap = end - q + 1;
    char *pat = (char *)malloc((size_t)cap + 1);
    int *at = (int *)malloc(sizeof(int) * ((size_t)cap + 1));
    if (!pat || !at) { free(pat); free(at); return; }
    if (decode(src, q, end, pat, at) >= 0) {
        TkRe re; TkReErr err;
        if (tkre_compile(&re, pat, &err) == 0) tkre_free(&re);
        else {
            int off = (err.pos >= 0) ? at[err.pos] : q;
            int line = lit->line, col = lit->col;
            for (int i = q; i < off; i++) {
                if (src[i] == '\n') { line++; col = 1; } else col++;
            }
            char msg[320];
            snprintf(msg, sizeof msg, "invalid regex pattern for str.%s: %s", fn, err.msg);
            char got[96];
            snprintf(got, sizeof got, "%.80s%s", pat, strlen(pat) > 80 ? "..." : "");
            /* No fix field: the intended pattern is not knowable (AGENTS 3.1). */
            diag_emit(DIAG_ERROR, E4075, off, line, col, msg,
                      "got", got,
                      "expected", "a pattern in toke's regex language (docs/stdlib/str.md, Regular expressions)",
                      NULL);
            (*count)++;
        }
    }
    free(pat); free(at);
}

static void walk(const Node *n, const char *src, int src_len, const SymbolTable *st, int *count) {
    if (!n) return;
    if (n->kind == NODE_CALL_EXPR) check_call(n, src, src_len, st, count);
    for (int i = 0; i < n->child_count; i++) walk(n->children[i], src, src_len, st, count);
}

int regex_check(const Node *ast, const char *src, int src_len, const SymbolTable *st) {
    int count = 0;
    if (!ast || !src || !st) return 0;
    int has_str = 0;
    for (int i = 0; i < st->count; i++)
        if (st->entries[i].module_path && !strcmp(st->entries[i].module_path, "std.str")) has_str = 1;
    if (has_str) walk(ast, src, src_len, st, &count);
    return count;
}
