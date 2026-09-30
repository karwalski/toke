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
 *
 * 121.45 (3): the pattern may also be an identifier bound by an immutable
 * `let p="literal"` in the same function, when that is provably the binding
 * the call reads -- see let_literal() for the (deliberately conservative)
 * conditions.  Anything it cannot prove is left to the run-time check.
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

/* ── 121.45 (3): a pattern named by an immutable `let` ─────────────────── */

static int same_ident(const Node *a, const Node *b, const char *src) {
    return a && b && a->kind == NODE_IDENT && b->kind == NODE_IDENT &&
           a->tok_len == b->tok_len &&
           memcmp(src + a->tok_start, src + b->tok_start, (size_t)a->tok_len) == 0;
}

/* An occurrence of the name whose parent is one of these is a READ of the
 * variable.  Any other parent -- a parameter, a match-arm or loop binding, a
 * `mut.` binding, an assignment, a closure, or a node kind added later -- may
 * bind or rebind the name, so the whole lookup gives up. */
static int is_read_context(const Node *parent, int idx) {
    switch (parent->kind) {
    case NODE_CALL_EXPR: case NODE_BINARY_EXPR: case NODE_UNARY_EXPR:
    case NODE_CAST_EXPR: case NODE_INDEX_EXPR: case NODE_EXPR_STMT:
    case NODE_RETURN_STMT: case NODE_ARRAY_LIT: case NODE_MAP_ENTRY:
    case NODE_PROPAGATE_EXPR:
        return 1;
    case NODE_FIELD_EXPR:   return 1;         /* child 1 is a member name: not a binder */
    case NODE_FIELD_INIT:   return idx >= 1;  /* child 0 is the field name */
    case NODE_BIND_STMT:    return idx >= 1;  /* the value, not the name */
    case NODE_MUT_BIND_STMT: case NODE_ASSIGN_STMT: case NODE_LOOP_INIT:
        return idx >= 1;
    case NODE_IF_STMT: case NODE_MATCH_STMT:  return idx == 0;
    default:                return 0;
    }
}

typedef struct {
    const Node *name;       /* the identifier being resolved */
    const char *src;
    const Node *bind;       /* the one `let name=...` found */
    const Node *bind_list;  /* the statement list that holds it */
    int binders, bad;
} LetScan;

static void let_scan(const Node *n, const Node *parent, int idx, LetScan *ls) {
    if (!n || ls->bad) return;
    if (same_ident(n, ls->name, ls->src)) {
        if (parent && parent->kind == NODE_BIND_STMT && idx == 0) {
            ls->binders++;
        } else if (!parent || !is_read_context(parent, idx)) {
            ls->bad = 1; return;
        }
    }
    if (n->kind == NODE_STMT_LIST) {
        for (int i = 0; i < n->child_count; i++) {
            const Node *c = n->children[i];
            if (c && c->kind == NODE_BIND_STMT && c->child_count >= 2 &&
                same_ident(c->children[0], ls->name, ls->src)) {
                ls->bind = c; ls->bind_list = n;
            }
        }
    }
    for (int i = 0; i < n->child_count; i++) let_scan(n->children[i], n, i, ls);
}

/* One past the last source byte any node of the subtree spans. */
static int subtree_end(const Node *n) {
    if (!n) return 0;
    int e = n->tok_start + n->tok_len;
    if (n->start > e) e = n->start;
    for (int i = 0; i < n->child_count; i++) {
        int c = subtree_end(n->children[i]);
        if (c > e) e = c;
    }
    return e;
}

/* The string literal an identifier pattern provably holds, or NULL.  Proved
 * when, within the enclosing function: the name has exactly one binder, an
 * immutable `let name="…"`; every other occurrence is a read; and the call
 * comes after that `let`, inside the statement list that holds it. */
static const Node *let_literal(const Node *fn, const Node *ident, const Node *call, const char *src) {
    if (!fn || !ident || ident->kind != NODE_IDENT) return NULL;
    LetScan ls = { ident, src, NULL, NULL, 0, 0 };
    let_scan(fn, NULL, 0, &ls);
    if (ls.bad || ls.binders != 1 || !ls.bind || !ls.bind_list) return NULL;
    const Node *val = ls.bind->children[1];
    if (!val || val->kind != NODE_STR_LIT) return NULL;
    /* inside the list, after the binding (offsets: the let ends at its value) */
    int after = val->tok_start + val->tok_len;
    if (call->start < after) return NULL;
    int list_end = subtree_end(ls.bind_list);
    if (call->start >= list_end) return NULL;
    return val;
}

/* Literals already reported through a `let` (one E4075 per literal, however
 * many calls read it). */
static int  g_seen[64];
static int  g_nseen;

static void check_call(const Node *call, const Node *fdecl, const char *src, int src_len, const SymbolTable *st, int *count) {
    if (call->child_count < 3) return;
    const Node *callee = call->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count < 2) return;
    if (!is_str_alias(callee->children[0], src, st)) return;
    const char *fn = NULL;
    for (int i = 0; REGEX_FNS[i]; i++)
        if (ident_is(callee->children[1], src, REGEX_FNS[i])) { fn = REGEX_FNS[i]; break; }
    if (!fn) return;
    const Node *lit = call->children[2];            /* (subject; pattern ...) */
    int via_let = 0;
    if (lit && lit->kind == NODE_IDENT) {
        lit = let_literal(fdecl, lit, call, src);
        via_let = 1;
        if (lit) {
            for (int i = 0; i < g_nseen; i++) if (g_seen[i] == lit->tok_start) return;
        }
    }
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
            if (via_let && g_nseen < (int)(sizeof g_seen / sizeof g_seen[0]))
                g_seen[g_nseen++] = lit->tok_start;
        }
    }
    free(pat); free(at);
}

static void walk(const Node *n, const Node *fn, const char *src, int src_len, const SymbolTable *st, int *count) {
    if (!n) return;
    if (n->kind == NODE_FUNC_DECL) fn = n;
    if (n->kind == NODE_CALL_EXPR) check_call(n, fn, src, src_len, st, count);
    for (int i = 0; i < n->child_count; i++) walk(n->children[i], fn, src, src_len, st, count);
}

int regex_check(const Node *ast, const char *src, int src_len, const SymbolTable *st) {
    int count = 0;
    if (!ast || !src || !st) return 0;
    int has_str = 0;
    for (int i = 0; i < st->count; i++)
        if (st->entries[i].module_path && !strcmp(st->entries[i].module_path, "std.str")) has_str = 1;
    g_nseen = 0;
    if (has_str) walk(ast, NULL, src, src_len, st, &count);
    return count;
}
