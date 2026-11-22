/*
 * lint.c — Static analysis lint rules for the toke reference compiler.
 *
 * Implements AST-only lint rules that run after a successful parse.
 * Each rule walks the AST recursively (switch on node->kind, recurse
 * into children) following the same pattern as ast_json.c.
 *
 * Base rules (45.1.2):
 *   unreachable-code      — statements after a return (<) in a statement list
 *   empty-fn-body         — function with an empty statement list body
 *   unused-import         — import alias never referenced in the AST
 *   redundant-bind        — `let x = x;` where both sides are the same ident
 *   unused-let            — `let x = expr;` where x is never referenced
 *   mutable-never-mutated — `let x=mut.v` where x is never reassigned
 *                            (131.104: fix only when the checker types the
 *                            `mut`-less program identically -- see
 *                            mut_drop_keeps_types; needs opts->recheck)
 *
 * Pattern rules (131.9, docs/compiler/lint-rules-v1.md "Pattern rules"):
 *   mut-flag-if            — `let x=mut.<lit>;` written only by the next if/el
 *   flag-soup              — ≥2 consecutive ifs each assigning x a literal
 *   string-concat-chain    — concat nested ≥ 2 deep
 *                            (fix dormant, TKC_LINT_CONCAT_FIX; 131.110:
 *                            only an all-str std.str chain the checker
 *                            typed -- see concat_chain_str_typed; needs
 *                            opts->typed and opts->recheck)
 *   single-use-let         — let used exactly once, in the next statement
 *                            (131.59: fix only when the checker typed the
 *                            use and the initialiser identically — see
 *                            inline_keeps_type; needs opts->typed)
 *   discarded-value-result — bare `x.set/push/append(...)` statement
 *                            (127.33: NOT the tail expression of an
 *                            expression-position if/el or mt arm — that
 *                            call IS the branch value, see pattern_walk)
 *                            (127.37: NOT a std.vec handle receiver —
 *                            `let x=v.new()` / `x:$vechandle` — by-reference,
 *                            the bare call mutates in place)
 *   loop-rebuilds-array    — lp whose body is only `acc=acc.append(f(i))`
 *   nested-if-ladder       — (131.64) `el{if(...){...}...}`: an else block
 *                            whose only statement is another if — write
 *                            `el if(...)`; fix splices the inner if up
 *
 * Fix model: every fixable diagnostic carries [span_start, span_end) and a
 * replacement string ("" = delete). main.c applies fixes right-to-left.
 *
 * AST facts this file relies on (verified against --dump-ast, tkc 2.8.0):
 *   - leaf nodes carry tok_start/tok_len; internal nodes carry `start`
 *     (BINARY_EXPR.start is the operator, CALL_EXPR.start is the `(`).
 *   - `arr.get(i)` parses as INDEX_EXPR(arr, i); `arr.len` as FIELD_EXPR.
 *   - identifiers inside "\(...)" interpolation are NOT in the AST — the
 *     STR_LIT is a leaf — so every reference count also scans string text.
 *   - IF_STMT children: cond, then STMT_LIST, [else STMT_LIST | IF_STMT].
 *   - LOOP_STMT children: LOOP_INIT(ident, init), cond, step, STMT_LIST.
 */

#include "lint.h"
#include "types.h"   /* 131.59: tk_type_identical, node->rtype */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <ctype.h>

#ifndef TKC_LINT_CONCAT_FIX
#define TKC_LINT_CONCAT_FIX 0   /* dormant until Epic 127.7 / 127.10 close */
#endif

/* ── Small string helpers ─────────────────────────────────────────────── */

static int src_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char *lstrndup(const char *s, int n)
{
    char *p = malloc((size_t)n + 1);
    if (!p) return NULL;
    memcpy(p, s, (size_t)n);
    p[n] = '\0';
    return p;
}

static char *lstrdup(const char *s)
{
    return lstrndup(s, (int)strlen(s));
}

/* Growable text buffer for building replacements / messages. */
typedef struct { char *buf; int len; int cap; } Str;

static int str_put(Str *s, const char *p, int n)
{
    if (s->len + n + 1 > s->cap) {
        int nc = s->cap ? s->cap * 2 : 64;
        while (nc < s->len + n + 1) nc *= 2;
        char *t = realloc(s->buf, (size_t)nc);
        if (!t) return -1;
        s->buf = t; s->cap = nc;
    }
    if (!s->buf) return -1;
    if (n > 0) memcpy(s->buf + s->len, p, (size_t)n);
    s->len += n;
    s->buf[s->len] = '\0';
    return 0;
}

static int str_puts(Str *s, const char *p) { return str_put(s, p, (int)strlen(p)); }

/* ── Result helpers ───────────────────────────────────────────────────── */

static int lint_push(LintResult *r, const char *rule_id, const char *message,
                     int offset, int line, int col, int severity,
                     int fixable, int span_start, int span_end,
                     const char *replacement)
{
    if (r->count >= r->cap) {
        int newcap = r->cap == 0 ? 16 : r->cap * 2;
        LintDiag *tmp = realloc(r->items, (size_t)newcap * sizeof(LintDiag));
        if (!tmp) return -1;
        r->items = tmp;
        r->cap   = newcap;
    }
    LintDiag *d = &r->items[r->count];
    memset(d, 0, sizeof *d);
    d->rule_id    = rule_id;
    d->message    = lstrdup(message ? message : "");
    if (!d->message) return -1;
    d->offset     = offset;
    d->line       = line;
    d->col        = col;
    d->severity   = severity;
    d->fixable    = fixable;
    d->span_start = span_start;
    d->span_end   = span_end;
    d->replacement = NULL;
    if (fixable && replacement) {
        d->replacement = lstrdup(replacement);
        if (!d->replacement) { free(d->message); return -1; }
    }
    r->count++;
    return 0;
}

/* ── Rule filtering ───────────────────────────────────────────────────── */

static int rule_enabled(const char *rule_id, const LintOptions *opts)
{
    if (!opts) return 1;

    /* If only_rules is set, the rule must appear in it */
    if (opts->only_rules && opts->only_count > 0) {
        int found = 0;
        for (int i = 0; i < opts->only_count; i++) {
            if (strcmp(rule_id, opts->only_rules[i]) == 0) { found = 1; break; }
        }
        if (!found) return 0;
    }

    /* If ignore_rules is set, skip if the rule appears */
    if (opts->ignore_rules && opts->ignore_count > 0) {
        for (int i = 0; i < opts->ignore_count; i++) {
            if (strcmp(rule_id, opts->ignore_rules[i]) == 0) return 0;
        }
    }

    return 1;
}

/* ── Identifier helpers ───────────────────────────────────────────────── */

static int is_ident(const Node *n)
{
    return n && n->kind == NODE_IDENT && n->tok_len > 0;
}

/* name_is — NODE_IDENT n has exactly the text (name, len). */
static int name_is(const Node *n, const char *name, int len, const char *src)
{
    return is_ident(n) && n->tok_len == len &&
           memcmp(src + n->tok_start, name, (size_t)len) == 0;
}

/*
 * ident_eq — return 1 if two NODE_IDENT nodes refer to the same name
 * by comparing their source text spans.
 */
static int ident_eq(const Node *a, const Node *b, const char *src)
{
    if (!is_ident(a) || !is_ident(b)) return 0;
    if (a->tok_len != b->tok_len) return 0;
    return memcmp(src + a->tok_start, src + b->tok_start, (size_t)a->tok_len) == 0;
}

/*
 * interp_ident_count — number of identifier tokens equal to (name, len)
 * inside "\(...)" interpolation segments of a string-literal token text.
 * Identifiers directly preceded by '.' (field/method names) are skipped.
 */
static int interp_ident_count(const char *s, int len, const char *name, int name_len)
{
    int count = 0;
    for (int i = 0; i + 1 < len; i++) {
        if (s[i] == '\\' && s[i + 1] == '(') {
            int depth = 1, j = i + 2;
            while (j < len && depth > 0) {
                char c = s[j];
                if (c == '(') { depth++; j++; continue; }
                if (c == ')') { depth--; if (depth == 0) break; j++; continue; }
                if (isalpha((unsigned char)c) || c == '_') {
                    int k = j;
                    while (k < len && (isalnum((unsigned char)s[k]) || s[k] == '_')) k++;
                    int prev_dot = (j > 0 && s[j - 1] == '.');
                    if (!prev_dot && k - j == name_len &&
                        memcmp(s + j, name, (size_t)name_len) == 0)
                        count++;
                    j = k;
                    continue;
                }
                j++;
            }
            i = j;
        } else if (s[i] == '\\') {
            i++;  /* skip the escaped character */
        }
    }
    return count;
}

static int str_lit_has_interp(const Node *n, const char *src)
{
    if (!n || n->kind != NODE_STR_LIT) return 0;
    for (int i = 0; i + 1 < n->tok_len; i++) {
        if (src[n->tok_start + i] == '\\') {
            if (src[n->tok_start + i + 1] == '(') return 1;
            i++;
        }
    }
    return 0;
}

/* ── Helper: check if an identifier name appears anywhere in a subtree ── */

static int ident_used_in(const Node *node, const char *name, int name_len,
                         const char *src)
{
    if (!node) return 0;

    /* Check NODE_IDENT and NODE_TYPE_IDENT leaves */
    if ((node->kind == NODE_IDENT || node->kind == NODE_TYPE_IDENT) &&
        node->tok_len == name_len &&
        memcmp(src + node->tok_start, name, (size_t)name_len) == 0) {
        return 1;
    }

    /* Interpolated identifiers live only in the string text (AST gap). */
    if (node->kind == NODE_STR_LIT && node->tok_len > 0 &&
        interp_ident_count(src + node->tok_start, node->tok_len, name, name_len) > 0)
        return 1;

    for (int i = 0; i < node->child_count; i++) {
        if (ident_used_in(node->children[i], name, name_len, src))
            return 1;
    }
    return 0;
}

/*
 * RefCount — context-aware reference census for one name inside a subtree.
 *   ast_refs    — genuine value references (not field names, not bindings)
 *   interp_refs — references inside "\(...)" string interpolation
 *   binds       — binding sites (let / mut / loop init / param / match arm)
 *   writes      — assignment targets
 *   first_ref   — the first AST reference found (document order)
 */
typedef struct {
    int ast_refs;
    int interp_refs;
    int binds;
    int writes;
    const Node *first_ref;
} RefCount;

static void count_refs(const Node *n, const Node *parent, int idx,
                       const char *name, int len, const char *src,
                       const Node *exclude, RefCount *rc)
{
    if (!n || n == exclude) return;

    if (n->kind == NODE_IDENT) {
        if (!name_is(n, name, len, src)) return;
        NodeKind pk = parent ? parent->kind : NODE_PROGRAM;
        if (pk == NODE_FIELD_EXPR && idx == 1) return;            /* field name */
        /* 127.289: a bare `lp(x=..)` init is a write, not a binder. */
        if (node_assigns(parent) && idx == 0) { rc->writes++; return; }
        if ((pk == NODE_BIND_STMT || pk == NODE_MUT_BIND_STMT ||
             pk == NODE_LOOP_INIT || pk == NODE_PARAM) && idx == 0) { rc->binds++; return; }
        if (pk == NODE_MATCH_ARM && idx == 1) { rc->binds++; return; }
        rc->ast_refs++;
        if (!rc->first_ref) rc->first_ref = n;
        return;
    }
    if (n->kind == NODE_STR_LIT && n->tok_len > 0) {
        rc->interp_refs += interp_ident_count(src + n->tok_start, n->tok_len, name, len);
        return;
    }
    for (int i = 0; i < n->child_count; i++)
        count_refs(n->children[i], n, i, name, len, src, exclude, rc);
}

/* ── Source-span helpers ──────────────────────────────────────────────── */

/* Leftmost byte covered by the subtree (leaf tokens and node starts > 0). */
static int subtree_min_start(const Node *n)
{
    if (!n) return -1;
    int m = -1;
    if (n->tok_len > 0) m = n->tok_start;
    if (n->start > 0 && (m < 0 || n->start < m)) m = n->start;
    for (int i = 0; i < n->child_count; i++) {
        int c = subtree_min_start(n->children[i]);
        if (c >= 0 && (m < 0 || c < m)) m = c;
    }
    return m;
}

/* Rightmost byte (exclusive) covered by any leaf token in the subtree. */
static int subtree_max_end(const Node *n)
{
    if (!n) return -1;
    int m = -1;
    if (n->tok_len > 0) m = n->tok_start + n->tok_len;
    else if (n->child_count == 0 && n->start > 0) m = n->start + 1;
    for (int i = 0; i < n->child_count; i++) {
        int c = subtree_max_end(n->children[i]);
        if (c > m) m = c;
    }
    return m;
}

/* Leaf node with the smallest token offset (for line/col anchoring). */
static const Node *leftmost_leaf(const Node *n)
{
    if (!n) return NULL;
    const Node *best = (n->tok_len > 0) ? n : NULL;
    for (int i = 0; i < n->child_count; i++) {
        const Node *c = leftmost_leaf(n->children[i]);
        if (c && (!best || c->tok_start < best->tok_start)) best = c;
    }
    return best;
}

/*
 * scan_stmt_end — from a statement's first byte, scan forward tracking
 * (), {} nesting and string literals until the terminating ';' at depth 0
 * (consumed) or a '}' at depth 0 (not consumed) or end of input.  Trailing
 * spaces/tabs and one newline are consumed so a deletion removes the line.
 */
static int scan_stmt_end(const char *src, int src_len, int from)
{
    int depth = 0, i = from;
    while (i < src_len) {
        char c = src[i];
        if (c == '"') {
            i++;
            while (i < src_len && src[i] != '"') { if (src[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (c == '(' || c == '{') depth++;
        else if (c == ')' || c == '}') {
            if (depth == 0) break;
            depth--;
        }
        else if (c == ';' && depth == 0) { i++; break; }
        i++;
    }
    while (i < src_len && (src[i] == ' ' || src[i] == '\t')) i++;
    if (i < src_len && src[i] == '\r') i++;
    if (i < src_len && src[i] == '\n') i++;
    return i;
}

/*
 * span_end_of_stmt — byte offset just past a statement's trailing
 * semicolon and newline.
 */
static int span_end_of_stmt(const Node *node, const char *src, int src_len)
{
    return scan_stmt_end(src, src_len, node->start);
}

/*
 * span_start_of_stmt — the statement's first byte, extended back over
 * leading spaces/tabs when the statement starts its line, so a deletion
 * takes the whole line rather than leaving indentation behind.
 */
static int span_start_of_stmt(const Node *node, const char *src)
{
    int s = node->start;
    int k = s;
    while (k > 0 && (src[k - 1] == ' ' || src[k - 1] == '\t')) k--;
    if (k == 0 || src[k - 1] == '\n') return k;
    return s;
}

/*
 * scan_call_end — src[open] == '(' ; return the offset just past the
 * matching ')', skipping string literals.  Returns -1 if unbalanced.
 */
static int scan_call_end(const char *src, int src_len, int open)
{
    if (open < 0 || open >= src_len || src[open] != '(') return -1;
    int depth = 0, i = open;
    while (i < src_len) {
        char c = src[i];
        if (c == '"') {
            i++;
            while (i < src_len && src[i] != '"') { if (src[i] == '\\') i++; i++; }
            i++;
            continue;
        }
        if (c == '(') depth++;
        else if (c == ')') { depth--; if (depth == 0) return i + 1; }
        i++;
    }
    return -1;
}

/* balanced — the slice has balanced () and {} outside string literals. */
static int balanced(const char *src, int s, int e)
{
    int dp = 0, db = 0;
    for (int i = s; i < e; i++) {
        char c = src[i];
        if (c == '"') {
            i++;
            while (i < e && src[i] != '"') { if (src[i] == '\\') i++; i++; }
            continue;
        }
        if (c == '(') dp++; else if (c == ')') { if (--dp < 0) return 0; }
        else if (c == '{') db++; else if (c == '}') { if (--db < 0) return 0; }
    }
    return dp == 0 && db == 0;
}

/* ── AST shape helpers ────────────────────────────────────────────────── */

/* A literal value: int/float/str/bool, or unary-minus over int/float. */
static int is_literal(const Node *n)
{
    if (!n) return 0;
    switch (n->kind) {
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_STR_LIT: case NODE_BOOL_LIT:
        return 1;
    case NODE_UNARY_EXPR:
        return n->child_count == 1 &&
               (n->children[0]->kind == NODE_INT_LIT ||
                n->children[0]->kind == NODE_FLOAT_LIT);
    default:
        return 0;
    }
}

static const Node *func_body(const Node *fn)
{
    if (!fn || fn->kind != NODE_FUNC_DECL || fn->child_count == 0) return NULL;
    const Node *b = fn->children[fn->child_count - 1];
    return (b && b->kind == NODE_STMT_LIST) ? b : NULL;
}

/* Import aliases of the program (first NODE_IDENT child of each import). */
typedef struct { const Node *ident[64]; int count; } Aliases;

static void collect_aliases(const Node *root, Aliases *a)
{
    a->count = 0;
    if (!root || root->kind != NODE_PROGRAM) return;
    for (int i = 0; i < root->child_count; i++) {
        const Node *imp = root->children[i];
        if (!imp || imp->kind != NODE_IMPORT) continue;
        const Node *alias = NULL;
        for (int j = 0; j < imp->child_count; j++) {
            if (is_ident(imp->children[j])) { alias = imp->children[j]; break; }
        }
        if (alias && a->count < 64) a->ident[a->count++] = alias;
    }
}

static int is_alias(const Aliases *a, const Node *n, const char *src)
{
    if (!is_ident(n)) return 0;
    for (int i = 0; i < a->count; i++)
        if (ident_eq(a->ident[i], n, src)) return 1;
    return 0;
}

/* Does the program declare a user function with this name (UFCS target)? */
static int program_declares_fn(const Node *root, const char *name, int len, const char *src)
{
    if (!root || root->kind != NODE_PROGRAM) return 0;
    for (int i = 0; i < root->child_count; i++) {
        const Node *d = root->children[i];
        if (d && d->kind == NODE_FUNC_DECL && d->child_count > 0 &&
            name_is(d->children[0], name, len, src))
            return 1;
    }
    return 0;
}

static const Node *method_call(const Node *n, const Node **base);
static int method_is(const Node *m, const char *name, const char *src);

/* count_assign_to — number of assignments (NODE_ASSIGN_STMT, or a bare
 * `lp(x=..)` init, 127.289) whose LHS is (name,len). */
static int count_assign_to(const Node *node, const char *name, int len, const char *src)
{
    if (!node) return 0;
    int c = 0;
    if (node_assigns(node) && node->child_count > 0 &&             /* 127.289 */
        name_is(node->children[0], name, len, src))
        c = 1;
    for (int i = 0; i < node->child_count; i++)
        c += count_assign_to(node->children[i], name, len, src);
    return c;
}

/*
 * has_assign_to — return 1 if the subtree contains a NODE_ASSIGN_STMT
 * whose LHS (children[0]) is a NODE_IDENT matching (name, name_len).
 */
/* bare_self_update — `name.set/push/append(...)` in statement position: the
 * author's evident intent is a write (discarded-value-result flags it). */
static int has_bare_self_update(const Node *node, const char *name, int len, const char *src)
{
    if (!node) return 0;
    if (node->kind == NODE_EXPR_STMT && node->child_count == 1) {
        const Node *base = NULL;
        const Node *m = method_call(node->children[0], &base);
        if (m && name_is(base, name, len, src) &&
            (method_is(m, "set", src) || method_is(m, "push", src) || method_is(m, "append", src)))
            return 1;
    }
    for (int i = 0; i < node->child_count; i++)
        if (has_bare_self_update(node->children[i], name, len, src)) return 1;
    return 0;
}

static int has_assign_to(const Node *node, const char *name, int name_len,
                         const char *src)
{
    return count_assign_to(node, name, name_len, src) > 0 ||
           has_bare_self_update(node, name, name_len, src);
}

/* method_call — n is CALL_EXPR(FIELD_EXPR(base, IDENT method), args...).
 * Returns the method ident node or NULL; *base receives the receiver. */
static const Node *method_call(const Node *n, const Node **base)
{
    if (!n || n->kind != NODE_CALL_EXPR || n->child_count < 1) return NULL;
    const Node *callee = n->children[0];
    if (!callee || callee->kind != NODE_FIELD_EXPR || callee->child_count != 2) return NULL;
    if (!is_ident(callee->children[1])) return NULL;
    if (base) *base = callee->children[0];
    return callee->children[1];
}

static int method_is(const Node *m, const char *name, const char *src)
{
    return m && name_is(m, name, (int)strlen(name), src);
}

/* ── Rule: unreachable-code ───────────────────────────────────────────── */

/*
 * In a NODE_STMT_LIST, if a NODE_RETURN_STMT appears at position i and
 * there are more children after it, those are unreachable.
 */
static int rule_unreachable_code(const Node *node, const char *src,
                                 const LintOptions *opts, LintResult *out)
{
    (void)src;
    if (!node) return 0;

    if (node->kind == NODE_STMT_LIST) {
        for (int i = 0; i < node->child_count; i++) {
            const Node *child = node->children[i];
            if (child && child->kind == NODE_RETURN_STMT &&
                i + 1 < node->child_count) {
                /* Flag each statement after the return */
                for (int j = i + 1; j < node->child_count; j++) {
                    const Node *unreachable = node->children[j];
                    if (unreachable && rule_enabled("unreachable-code", opts)) {
                        if (lint_push(out, "unreachable-code",
                                      "statement is unreachable after return",
                                      unreachable->start,
                                      unreachable->line, unreachable->col,
                                      LINT_WARNING, 0,
                                      unreachable->start, unreachable->start, NULL) < 0)
                            return -1;
                    }
                }
                break;  /* only flag once per statement list */
            }
        }
    }

    /* Recurse into children */
    for (int i = 0; i < node->child_count; i++) {
        if (rule_unreachable_code(node->children[i], src, opts, out) < 0)
            return -1;
    }
    return 0;
}

/* ── Rule: empty-fn-body ──────────────────────────────────────────────── */

/*
 * A NODE_FUNC_DECL whose body (last child, a NODE_STMT_LIST) has zero
 * children.  We skip functions with zero children entirely (forward decls).
 */
static int rule_empty_fn_body(const Node *node, const char *src,
                              const LintOptions *opts, LintResult *out)
{
    (void)src;
    if (!node) return 0;

    if (node->kind == NODE_FUNC_DECL && node->child_count > 0) {
        const Node *last = node->children[node->child_count - 1];
        if (last && last->kind == NODE_STMT_LIST && last->child_count == 0) {
            if (rule_enabled("empty-fn-body", opts)) {
                if (lint_push(out, "empty-fn-body",
                              "function has an empty body",
                              node->start, node->line, node->col, LINT_WARNING,
                              0, node->start, node->start, NULL) < 0)
                    return -1;
            }
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        if (rule_empty_fn_body(node->children[i], src, opts, out) < 0)
            return -1;
    }
    return 0;
}

/* ── Qualified-type references to an import alias (127.81) ───────────── */

/*
 * A qualified type `alias.$name` is one NODE_TYPE_IDENT whose token is
 * *name*, marked `op == TK_DOT`.  Its alias has no node of its own, so
 * `ident_used_in` -- which compares node tokens -- never sees it.  That gap
 * made `unused-import` advise deleting an import the program depends on, and
 * the import list is not merely a name-resolution scope: it is the
 * compiler's module manifest.  Three codegen consumers key off it directly --
 *   - resolve_stdlib_deps_imports_only (src/stdlib_deps.c) picks the C
 *     runtime files to compile in, so a dropped import silently drops the
 *     module's implementation out of the link set;
 *   - register_tki_struct_types (src/llvm.c) loads stdlib/<mod>.tki to get
 *     record field -> GEP indices, so a dropped import makes field access
 *     fall back to index 0 -- a silently wrong field, not a diagnostic;
 *   - prepass_load_tki / resolve_stdlib_call map the alias to a C symbol.
 * Deleting an import is therefore a semantic change to the emitted binary.
 *
 * The parser keeps the alias on the node (qual_start/qual_len, 131.81); this
 * compares it.  It used to be recovered by re-reading the source text before
 * the type token.
 */
static int qual_type_alias_is(const Node *n, const char *name, int name_len,
                              const char *src)
{
    if (!n || n->kind != NODE_TYPE_IDENT || n->op != TK_DOT) return 0;
    if (n->qual_len != name_len) return 0;
    return memcmp(src + n->qual_start, name, (size_t)name_len) == 0;
}

/* Does (name, name_len) appear as the qualifier of any qualified type? */
static int qual_type_used_in(const Node *node, const char *name, int name_len,
                             const char *src)
{
    int i;
    if (!node) return 0;
    if (qual_type_alias_is(node, name, name_len, src)) return 1;
    for (i = 0; i < node->child_count; i++)
        if (qual_type_used_in(node->children[i], name, name_len, src))
            return 1;
    return 0;
}

/* ── Unqualified references to a type no local declaration provides (137.8)
 *
 * An import does not only bring its alias into scope. Its record and sum
 * types are also reachable UNQUALIFIED: `i=t:shared.types;` followed by
 * `f=code(e:$lokeerr):i64{…}` resolves `$lokeerr` through the import, and
 * the alias `t` never appears again. Deleting that import is E3011
 * (`identifier 'lokeerr' is not declared`) on the next compile — measured on
 * loke as 197 imports deleted in 119 files, with `--fix` reporting success.
 *
 * Lint runs on the parsed AST alone, before name resolution, so it cannot say
 * WHICH import supplies such a name. What it can say exactly is whether the
 * file refers to any type name that nothing in this file declares. When it
 * does, no import can be proved unused, and the rule stays silent: a fix
 * must never change what the program means (127.81), and a warning that
 * cannot be acted on safely is not one to offer.
 *
 * Locally declared names are the `t=$name{…}` declarations and every field
 * label inside them — a sum type's variants are fields, and match arms name
 * them as `$variant:binding` — plus the names every program has without an
 * import: names.c's `predefined[]` seed (true/false/…/ok/err/none) and the
 * parser's scalar list (`is_scalar`, which also accepts `$str`, `$f64`, …).
 * Keep this list in step with those two; a name missing here only makes the
 * rule more cautious, never wrong.
 */
static const char *const lint_predefined_types[] = {
    /* src/names.c predefined[] */
    "true", "false", "bool", "i64", "u64", "f64", "str", "void",
    "ok", "err", "none",
    /* src/parser.c is_scalar() */
    "u8", "u16", "u32", "i8", "i16", "i32", "f32", "byte", "Str", "Byte",
    NULL
};
typedef struct { const char *s[256]; int n[256]; int count; int overflow; } NameSet;

static void nameset_add(NameSet *ns, const char *s, int n)
{
    if (n <= 0) return;
    if (ns->count >= 256) { ns->overflow = 1; return; }
    ns->s[ns->count] = s; ns->n[ns->count] = n; ns->count++;
}

static int nameset_has(const NameSet *ns, const char *s, int n)
{
    for (int i = 0; i < ns->count; i++)
        if (ns->n[i] == n && memcmp(ns->s[i], s, (size_t)n) == 0) return 1;
    return 0;
}

/* The identifier text at a node's token, skipping a leading '$'. */
static int node_name(const Node *n, const char *src, const char **out)
{
    int p = n->tok_start, e;
    if (src[p] == '$') p++;
    e = p;
    while (isalnum((unsigned char)src[e]) || src[e] == '_') e++;
    *out = src + p;
    return e - p;
}

/* 138.39: only the declared NAME (the TYPE_DECL's own TYPE_IDENT child) and
 * the field labels are declarations.  A TYPE_IDENT inside a field's type --
 * `t=$holder{items:@$item}` -- is a REFERENCE; collecting it as declared
 * made `$item` look local, so the import that supplies it was reported
 * unused and `--fix` deleted it (E3011 on the next check). */
static void collect_local_type_names(const Node *n, const char *src,
                                     NameSet *ns, int in_type_decl)
{
    const char *s; int len;
    if (!n) return;
    if (n->kind == NODE_TYPE_DECL) {
        in_type_decl = 1;
        if (n->child_count > 0 && n->children[0]
            && n->children[0]->kind == NODE_TYPE_IDENT
            && n->children[0]->op != TK_DOT) {
            len = node_name(n->children[0], src, &s);
            nameset_add(ns, s, len);
        }
    }
    if (in_type_decl && n->kind == NODE_FIELD) {
        len = node_name(n, src, &s);
        nameset_add(ns, s, len);
    }
    for (int i = 0; i < n->child_count; i++)
        collect_local_type_names(n->children[i], src, ns, in_type_decl);
}

static int foreign_type_ref_in(const Node *n, const char *src, const NameSet *ns)
{
    const char *s; int len;
    if (!n) return 0;
    if ((n->kind == NODE_TYPE_IDENT && n->op != TK_DOT) ||
        n->kind == NODE_STRUCT_LIT) {
        len = node_name(n, src, &s);
        if (len > 0 && !nameset_has(ns, s, len)) return 1;
    }
    for (int i = 0; i < n->child_count; i++)
        if (foreign_type_ref_in(n->children[i], src, ns)) return 1;
    return 0;
}

/* Does any non-import declaration name a type this file does not declare? */
static int program_has_foreign_type_ref(const Node *root, const char *src)
{
    NameSet ns;
    memset(&ns, 0, sizeof ns);
    for (int i = 0; lint_predefined_types[i]; i++)
        nameset_add(&ns, lint_predefined_types[i],
                    (int)strlen(lint_predefined_types[i]));
    for (int i = 0; i < root->child_count; i++)
        collect_local_type_names(root->children[i], src, &ns, 0);
    if (ns.overflow) return 1;   /* cannot tell — assume the worst */
    for (int i = 0; i < root->child_count; i++) {
        const Node *d = root->children[i];
        if (!d || d->kind == NODE_IMPORT) continue;
        if (foreign_type_ref_in(d, src, &ns)) return 1;
    }
    return 0;
}

/* 138.39: does the source with the n import statements [ds[k], de[k])
 * deleted still lex, parse and type-check with no error?  The spans are in
 * source order and do not overlap (one per import declaration). */
static int import_drop_rechecks(const LintOptions *o, const char *src,
                                int src_len, const int *ds, const int *de,
                                int n)
{
    char *p = malloc((size_t)src_len + 1);
    if (!p) return 0;
    int len = 0;
    for (int k = 0; k < src_len; k++) {
        int del = 0;
        for (int j = 0; j < n && !del; j++) del = (k >= ds[j] && k < de[j]);
        if (!del) p[len++] = src[k];
    }
    p[len] = '\0';
    void *h = NULL;
    const Node *nr = o->recheck(o->recheck_ctx, p, len, &h);
    if (o->recheck_free) o->recheck_free(o->recheck_ctx, h);
    free(p);
    return nr != NULL;
}

/* ── Rule: unused-import ──────────────────────────────────────────────── */

/*
 * NODE_IMPORT children: the alias (NODE_IDENT) followed by the module path.
 * For each import, check whether the alias appears anywhere else in the AST
 * (interpolation text included).
 */
static int rule_unused_import(const Node *root, const char *src,
                              int src_len,
                              const LintOptions *opts, LintResult *out)
{
    if (!root || root->kind != NODE_PROGRAM) return 0;

    /* 137.8: an unqualified `$type` this file does not declare arrives through
     * SOME import, and lint cannot tell which -- so none is provably unused.
     * This holds in typed files too: the re-check below sees only what
     * --check judges, and codegen still needs imports for shapes --check
     * does not judge (an array of a foreign record, a T!E payload, ...). */
    if (program_has_foreign_type_ref(root, src)) return 0;

    /* 138.39: when the program type-checks and the caller can re-check a
     * candidate (LintOptions.recheck), an import with no syntactic use is
     * reported only if the program WITHOUT it -- and without every import
     * already reported -- still checks clean.  This only ever withdraws a
     * report: it counts a field read whose layout only the import
     * establishes (`r.text` on the `$res` another module returns, E4034
     * without it, 138.40).  A use only codegen needs is not seen. */
    int probe = opts && opts->typed && opts->recheck;
    int drop_s[64], drop_e[64], drop_n = 0;

    for (int i = 0; i < root->child_count; i++) {
        const Node *imp = root->children[i];
        if (!imp || imp->kind != NODE_IMPORT) continue;

        const Node *alias = NULL;
        for (int j = 0; j < imp->child_count; j++)
            if (is_ident(imp->children[j])) { alias = imp->children[j]; break; }
        if (!alias) continue;

        const char *name = src + alias->tok_start;
        int name_len = alias->tok_len;

        int used = 0;
        for (int j = 0; j < root->child_count; j++) {
            if (j == i) continue;
            const Node *decl = root->children[j];
            if (!decl || decl->kind == NODE_IMPORT) continue;
            if (ident_used_in(decl, name, name_len, src)) { used = 1; break; }
            /* 127.81: a type-only use (`p:alias.$rec`) leaves no ident node. */
            if (qual_type_used_in(decl, name, name_len, src)) { used = 1; break; }
        }

        if (!used && probe) {
            int ss = span_start_of_stmt(imp, src);
            int se = span_end_of_stmt(imp, src, src_len);
            if (drop_n >= 64) used = 1;     /* cannot probe: keep it */
            else {
                drop_s[drop_n] = ss; drop_e[drop_n] = se;
                if (import_drop_rechecks(opts, src, src_len, drop_s, drop_e,
                                         drop_n + 1))
                    drop_n++;
                else
                    used = 1;
            }
        }

        if (!used && rule_enabled("unused-import", opts)) {
            char msg[256];
            snprintf(msg, sizeof msg, "imported module '%.*s' is never used",
                     name_len, name);
            if (lint_push(out, "unused-import", msg,
                          imp->start, imp->line, imp->col, LINT_WARNING,
                          1, span_start_of_stmt(imp, src),
                          span_end_of_stmt(imp, src, src_len), "") < 0)
                return -1;
        }
    }
    return 0;
}

/* ── Rule: redundant-bind ─────────────────────────────────────────────── */

/*
 * `let x = x;` — a NODE_BIND_STMT where child[0] (the name) and
 * the initialiser are both NODE_IDENT with the same text.
 */
static int rule_redundant_bind(const Node *node, const char *src,
                               int src_len,
                               const LintOptions *opts, LintResult *out)
{
    if (!node) return 0;

    if (node->kind == NODE_BIND_STMT && node->child_count >= 2) {
        const Node *lhs = node->children[0];
        const Node *rhs = node->children[node->child_count - 1];
        if (ident_eq(lhs, rhs, src) && rule_enabled("redundant-bind", opts)) {
            if (lint_push(out, "redundant-bind",
                          "binding assigns a variable to itself",
                          node->start, node->line, node->col, LINT_WARNING,
                          1, span_start_of_stmt(node, src),
                          span_end_of_stmt(node, src, src_len), "") < 0)
                return -1;
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        if (rule_redundant_bind(node->children[i], src, src_len, opts, out) < 0)
            return -1;
    }
    return 0;
}

/* ── Initialiser inertness (127.82) ──────────────────────────────────── */

/*
 * An unused *name* is not an unused *expression*.  `unused-let` used to
 * offer a fix that deleted the whole statement — initialiser included — so
 * `let ok=fs.mkdir(dir);` and `let st=proc.wait(pid);` had the directory
 * creation and the process wait deleted along with the name nobody read.
 * The diagnostic is right (the binding really is dead); the rewrite was not.
 *
 * So the fix is now offered only for an initialiser that is provably inert:
 * an allowlist of node kinds that cannot do anything but compute a value.
 * Anything else — every call, propagate, spawn, closure, index or cast, and
 * any node kind added after this was written — keeps the warning and loses
 * the automatic fix, which a human can still apply by hand after reading it.
 *
 * Deliberately an allowlist, not a denylist: a new effectful node kind must
 * be opted *in* to auto-deletion rather than silently inheriting it.
 */
static int expr_is_inert(const Node *n)
{
    int i;
    if (!n) return 0;

    switch (n->kind) {
    /* Leaves: literals and plain names. */
    case NODE_INT_LIT:
    case NODE_FLOAT_LIT:
    case NODE_STR_LIT:
    case NODE_BOOL_LIT:
    case NODE_IDENT:
    case NODE_TYPE_IDENT:
    case NODE_TYPE_EXPR:
        return 1;

    /* Pure structure over inert operands. */
    case NODE_BINARY_EXPR:
    case NODE_UNARY_EXPR:
    case NODE_ARRAY_LIT:
    case NODE_MAP_LIT:
    case NODE_MAP_ENTRY:
    case NODE_STRUCT_LIT:
    case NODE_FIELD_INIT:
    case NODE_FIELD_EXPR:
        break;

    /*
     * Everything else is treated as effectful.  NODE_CALL_EXPR is the case
     * this rule exists for; NODE_INDEX_EXPR is excluded because `a.get(i)`
     * parses as one (see the header note) and a user `get` may do anything.
     */
    default:
        return 0;
    }

    for (i = 0; i < n->child_count; i++)
        if (!expr_is_inert(n->children[i])) return 0;
    return 1;
}

/* ── Rule: unused-let ─────────────────────────────────────────────────── */

/*
 * In a function body, find NODE_BIND_STMT nodes whose bound name is never
 * referenced anywhere else in the function body (interpolation included).
 */
static int check_unused_binds(const Node *body, const char *src,
                               int src_len,
                               const LintOptions *opts, LintResult *out)
{
    if (!body || body->kind != NODE_STMT_LIST) return 0;

    for (int i = 0; i < body->child_count; i++) {
        const Node *stmt = body->children[i];
        if (!stmt || stmt->kind != NODE_BIND_STMT) continue;
        if (stmt->child_count < 1) continue;

        const Node *lhs = stmt->children[0];
        if (!is_ident(lhs)) continue;

        const char *name = src + lhs->tok_start;
        int name_len = lhs->tok_len;

        int used = 0;
        for (int j = 0; j < body->child_count; j++) {
            if (j == i) continue;
            if (ident_used_in(body->children[j], name, name_len, src)) { used = 1; break; }
        }

        if (!used && rule_enabled("unused-let", opts)) {
            char msg[256];
            /*
             * 127.82: only delete the statement automatically when its
             * initialiser cannot have done anything.  Otherwise warn and
             * leave the rewrite to a human — deleting the name would also
             * delete the effect the call was there for.
             */
            const Node *init = stmt->children[stmt->child_count - 1];
            int inert = (stmt->child_count >= 2) && expr_is_inert(init);

            snprintf(msg, sizeof msg,
                     inert ? "binding '%.*s' is never used"
                           : "binding '%.*s' is never used, but its "
                             "initialiser may have an effect: remove the "
                             "binding by hand, or keep the call as a "
                             "statement",
                     name_len, name);
            if (lint_push(out, "unused-let", msg,
                          stmt->start, stmt->line, stmt->col, LINT_WARNING,
                          inert, span_start_of_stmt(stmt, src),
                          span_end_of_stmt(stmt, src, src_len),
                          inert ? "" : NULL) < 0)
                return -1;
        }
    }
    return 0;
}

static int rule_unused_let(const Node *node, const char *src,
                            int src_len,
                            const LintOptions *opts, LintResult *out)
{
    if (!node) return 0;

    if (node->kind == NODE_FUNC_DECL && node->child_count > 0) {
        if (check_unused_binds(func_body(node), src, src_len, opts, out) < 0)
            return -1;
    }

    for (int i = 0; i < node->child_count; i++) {
        if (rule_unused_let(node->children[i], src, src_len, opts, out) < 0)
            return -1;
    }
    return 0;
}

/* ── Rule: mutable-never-mutated ──────────────────────────────────────── */

/*
 * A NODE_MUT_BIND_STMT declares a mutable binding.  If no NODE_ASSIGN_STMT
 * in the enclosing function body targets the same identifier, the binding
 * could be immutable.  Fix: delete the `mut.` prefix (span replacement).
 *
 * 131.104: deleting `mut.` is NOT type-transparent.  The checker types a
 * binding by its mutability (types.c bind_init_type): an int / float / bool
 * literal, a negated literal and a std call's scalar return type an
 * IMMUTABLE `let` only (127.229, 127.230, 127.248, 138.3), and a `mut` empty
 * `@()` takes its element type from its first append.  So
 * `let x=mut.5; g(x)` into a u64 parameter checks clean and prints 5, while
 * the fixed `let x=5; g(x)` is E4031 -- a compiling program turned into a
 * non-compiling one -- and on arithmetic beside a u64 the two can be
 * different operations.  Rather than restate the checker's rules here (they
 * move: 127.315 widens one of them), the fix asks the checker itself
 * (LintOptions.recheck) and is offered only when every one of these holds
 * (AGENTS.md section 6 -- absent if uncertain):
 *   - the original program type-checked cleanly (opts->typed) and a re-check
 *     callback is available;
 *   - the name is bound exactly once in the function (no shadowing, so every
 *     reference below is this binding);
 *   - the program with this `mut.` deleted -- together with every deletion
 *     already accepted in this pass, so combined fixes are judged together
 *     -- lexes, parses and type-checks cleanly;
 *   - the two ASTs are the same shape (only MUT_BIND -> BIND differs) and the
 *     checker gave every node the same type: tk_type_identical, or both
 *     absent, or both TY_UNKNOWN;
 *   - the binding's type is OBSERVED: at least one AST reference carries a
 *     type in both programs.  A reference only inside "\(...)"
 *     interpolation is not in the AST, so a binding used only there (or
 *     only as an untyped method receiver) keeps the hint without a fix.
 */
static int count_bind_sites(const Node *n, const Node *parent, int idx,
                            const char *name, int len, const char *src,
                            const Node **mut_decl);

typedef struct {
    const Node        *root;
    const char        *src;
    int                src_len;
    const LintOptions *opts;
    LintResult        *out;
    int                acc[64];   /* starts of accepted `mut.` deletions */
    int                acc_n;
} MutCtx;

/* Same checker type: both absent, both TY_UNKNOWN, or tk_type_identical. */
static int mut_same_type(const Type *a, const Type *b)
{
    if (!a || !b) return !a && !b;
    if (a->kind == TY_UNKNOWN || b->kind == TY_UNKNOWN)
        return a->kind == TY_UNKNOWN && b->kind == TY_UNKNOWN;
    return tk_type_identical(a, b);
}

/* Is `n` (child idx of `p`) a value reference to (name,len)?  count_refs'
 * classification: not a field name, binder or assignment target. */
static int mut_is_ref(const Node *n, const Node *p, int idx,
                      const char *name, int len, const char *src)
{
    if (!name_is(n, name, len, src) || !p) return 0;
    if (p->kind == NODE_FIELD_EXPR && idx == 1) return 0;
    if ((p->kind == NODE_BIND_STMT || p->kind == NODE_MUT_BIND_STMT ||
         p->kind == NODE_LOOP_INIT || p->kind == NODE_PARAM) && idx == 0) return 0;
    if (p->kind == NODE_MATCH_ARM && idx == 1) return 0;
    if (p->kind == NODE_ASSIGN_STMT && idx == 0) return 0;
    return 1;
}

/* Walk the original tree `a` and the re-checked tree `b` in step.  Returns 1
 * when they are the same shape and every node has the same type; sets
 * *observed when a reference to (name,len) inside `fn` is typed in both. */
static int mut_tree_same(const MutCtx *m, const Node *a, const Node *b,
                         const Node *pa, int idx, const Node *fn, int in_fn,
                         const char *name, int len, int *observed)
{
    if (!a || !b) return !a && !b;
    if (a->kind != b->kind &&
        !(a->kind == NODE_MUT_BIND_STMT && b->kind == NODE_BIND_STMT)) return 0;
    if (a->child_count != b->child_count) return 0;
    if (!mut_same_type(a->rtype, b->rtype)) return 0;
    if (a == fn) in_fn = 1;
    if (in_fn && a->kind == NODE_IDENT && a->rtype && b->rtype &&
        mut_is_ref(a, pa, idx, name, len, m->src))
        *observed = 1;
    for (int i = 0; i < a->child_count; i++)
        if (!mut_tree_same(m, a->children[i], b->children[i], a, i, fn, in_fn,
                           name, len, observed))
            return 0;
    return 1;
}

/* The source with every accepted `mut.` and the one at `fs` deleted. */
static char *mut_probe_src(const MutCtx *m, int fs, int *plen)
{
    char *p = malloc((size_t)m->src_len + 1);
    if (!p) return NULL;
    int n = 0;
    for (int k = 0; k < m->src_len; k++) {
        int del = (k == fs);
        for (int j = 0; j < m->acc_n && !del; j++) del = (k == m->acc[j]);
        if (del) { k += 3; continue; }
        p[n++] = m->src[k];
    }
    p[n] = '\0';
    *plen = n;
    return p;
}

static int mut_drop_keeps_types(MutCtx *m, const Node *fn, const Node *ident, int fs)
{
    const LintOptions *o = m->opts;
    if (!o || !o->typed || !o->recheck || m->acc_n >= 64) return 0;
    const char *name = m->src + ident->tok_start;
    int len = ident->tok_len;
    const Node *md = NULL;
    if (count_bind_sites(fn, NULL, 0, name, len, m->src, &md) != 1) return 0;
    int plen = 0;
    char *probe = mut_probe_src(m, fs, &plen);
    if (!probe) return 0;
    void *h = NULL;
    const Node *nr = o->recheck(o->recheck_ctx, probe, plen, &h);
    int observed = 0;
    int ok = nr && mut_tree_same(m, m->root, nr, NULL, 0, fn, 0, name, len, &observed)
             && observed;
    if (o->recheck_free) o->recheck_free(o->recheck_ctx, h);
    free(probe);
    if (ok) m->acc[m->acc_n++] = fs;
    return ok;
}

/* Byte span of the `mut.` (or `MUT.`) after `name=` in a binding, or 0. */
static int mut_prefix_span(const Node *ident, const char *src, int src_len, int *fs)
{
    int k = ident->tok_start + ident->tok_len;
    while (k < src_len && src[k] == ' ') k++;
    if (k >= src_len || src[k] != '=') return 0;
    k++;
    while (k < src_len && src[k] == ' ') k++;
    if (k + 4 <= src_len &&
        (memcmp(src + k, "mut.", 4) == 0 || memcmp(src + k, "MUT.", 4) == 0)) {
        *fs = k;
        return 1;
    }
    return 0;
}

static int mutable_never_mutated_walk(MutCtx *m, const Node *node)
{
    if (!node) return 0;

    const Node *body = (node->kind == NODE_FUNC_DECL) ? func_body(node) : NULL;
    for (int i = 0; body && i < body->child_count; i++) {
        const Node *stmt = body->children[i];
        if (!stmt || stmt->kind != NODE_MUT_BIND_STMT || stmt->child_count == 0) continue;
        const Node *ident = stmt->children[0];
        if (!is_ident(ident)) continue;
        const char *name = m->src + ident->tok_start;
        int name_len = ident->tok_len;
        if (has_assign_to(body, name, name_len, m->src) ||
            !rule_enabled("mutable-never-mutated", m->opts)) continue;
        char msg[256];
        snprintf(msg, sizeof msg,
            "mutable binding '%.*s' is never reassigned; "
            "consider removing 'mut'", name_len, name);
        int fs = 0;
        int fixable = mut_prefix_span(ident, m->src, m->src_len, &fs) &&
                      mut_drop_keeps_types(m, node, ident, fs);   /* 131.104 */
        if (lint_push(m->out, "mutable-never-mutated", msg,
                      stmt->start, stmt->line, stmt->col, LINT_WARNING,
                      fixable, fixable ? fs : 0, fixable ? fs + 4 : 0, "") < 0)
            return -1;
    }

    for (int i = 0; i < node->child_count; i++)
        if (mutable_never_mutated_walk(m, node->children[i]) < 0) return -1;
    return 0;
}

static int rule_mutable_never_mutated(const Node *ast, const char *src, int src_len,
                                      const LintOptions *opts, LintResult *out)
{
    MutCtx m;
    memset(&m, 0, sizeof m);
    m.root = ast; m.src = src; m.src_len = src_len; m.opts = opts; m.out = out;
    return mutable_never_mutated_walk(&m, ast);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Pattern rules (Epic 131.9)
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
    const Node   *root;
    const char   *src;
    int           src_len;
    const LintOptions *opts;
    LintResult   *out;
    Aliases       aliases;
    /* 131.64: inner ifs of an already-reported nested-if ladder */
    const Node   *ladder_seen[64];
    int           ladder_seen_n;
} PatCtx;

/* single_assign — STMT_LIST containing exactly one `name = rhs` whose rhs
 * does not mention name.  Returns the ASSIGN node or NULL. */
static const Node *single_assign(const Node *list, const char *name, int len, const char *src)
{
    if (!list || list->kind != NODE_STMT_LIST || list->child_count != 1) return NULL;
    const Node *a = list->children[0];
    if (!a || a->kind != NODE_ASSIGN_STMT || a->child_count != 2) return NULL;
    if (!name_is(a->children[0], name, len, src)) return NULL;
    if (ident_used_in(a->children[1], name, len, src)) return NULL;
    return a;
}

/*
 * if_assigns_only — every leaf branch of the if / el-if chain is exactly one
 * assignment `name=<rhs>` and no condition mentions name.  *no_else is set
 * when some chain tail lacks an else branch (the flag keeps its literal).
 * *branches counts leaf branches that assign.
 */
static int if_assigns_only(const Node *ifn, const char *name, int len,
                           const char *src, int *no_else, int *branches)
{
    if (!ifn || ifn->kind != NODE_IF_STMT || ifn->child_count < 2) return 0;
    if (ident_used_in(ifn->children[0], name, len, src)) return 0;
    if (!single_assign(ifn->children[1], name, len, src)) return 0;
    (*branches)++;
    if (ifn->child_count == 2) { *no_else = 1; return 1; }
    const Node *els = ifn->children[2];
    if (!els) { *no_else = 1; return 1; }
    if (els->kind == NODE_IF_STMT)
        return if_assigns_only(els, name, len, src, no_else, branches);
    if (els->kind == NODE_STMT_LIST) {
        if (single_assign(els, name, len, src)) { (*branches)++; return 1; }
        if (els->child_count == 1 && els->children[0] &&
            els->children[0]->kind == NODE_IF_STMT)
            return if_assigns_only(els->children[0], name, len, src, no_else, branches);
    }
    return 0;
}

/* Rule 1: mut-flag-if */
static int check_mut_flag_if(PatCtx *c, const Node *list, int i, const Node *body)
{
    const Node *decl = list->children[i];
    if (i + 1 >= list->child_count) return 0;
    const Node *ifn = list->children[i + 1];
    if (!ifn || ifn->kind != NODE_IF_STMT) return 0;
    if (decl->child_count < 2 || !is_ident(decl->children[0])) return 0;
    if (!is_literal(decl->children[decl->child_count - 1])) return 0;

    const Node *id = decl->children[0];
    const char *name = c->src + id->tok_start;
    int len = id->tok_len;

    int no_else = 0, branches = 0;
    if (!if_assigns_only(ifn, name, len, c->src, &no_else, &branches)) return 0;
    /* Every write to the flag must live inside this if. */
    if (count_assign_to(body, name, len, c->src) != count_assign_to(ifn, name, len, c->src))
        return 0;
    if (!rule_enabled("mut-flag-if", c->opts)) return 0;

    char msg[320];
    if (no_else)
        snprintf(msg, sizeof msg,
            "mutable flag '%.*s' is only assigned by the if that follows — "
            "bind the expression-if directly: `let %.*s=if(cond){value}el{%.*s}`",
            len, name, len, name,
            decl->children[decl->child_count - 1]->tok_len,
            c->src + decl->children[decl->child_count - 1]->tok_start);
    else
        snprintf(msg, sizeof msg,
            "mutable flag '%.*s' is only assigned by the if/el that follows — "
            "bind the expression-if directly: `let %.*s=if(cond){a}el{b}`",
            len, name, len, name);
    return lint_push(c->out, "mut-flag-if", msg, decl->start, decl->line, decl->col,
                     LINT_WARNING, 0, decl->start, span_end_of_stmt(ifn, c->src, c->src_len), NULL);
}

/* flag_set_if — `if(cond){x=<lit>}` or `if(cond){x=<lit>}el{x=<lit>}`;
 * returns the assigned ident node or NULL. */
static const Node *flag_set_if(const Node *ifn, const char *src)
{
    if (!ifn || ifn->kind != NODE_IF_STMT || ifn->child_count < 2) return NULL;
    const Node *then = ifn->children[1];
    if (!then || then->kind != NODE_STMT_LIST || then->child_count != 1) return NULL;
    const Node *a = then->children[0];
    if (!a || a->kind != NODE_ASSIGN_STMT || a->child_count != 2) return NULL;
    if (!is_ident(a->children[0]) || !is_literal(a->children[1])) return NULL;
    const Node *id = a->children[0];
    if (ident_used_in(ifn->children[0], src + id->tok_start, id->tok_len, src)) return NULL;
    if (ifn->child_count >= 3 && ifn->children[2]) {
        const Node *els = ifn->children[2];
        if (els->kind != NODE_STMT_LIST || els->child_count != 1) return NULL;
        const Node *b = els->children[0];
        if (!b || b->kind != NODE_ASSIGN_STMT || b->child_count != 2) return NULL;
        if (!ident_eq(b->children[0], id, src) || !is_literal(b->children[1])) return NULL;
    }
    return id;
}

/* Rule 2: flag-soup — returns the number of statements consumed. */
static int check_flag_soup(PatCtx *c, const Node *list, int i, int *err)
{
    const Node *first = flag_set_if(list->children[i], c->src);
    if (!first) return 1;
    int n = 1;
    while (i + n < list->child_count) {
        const Node *id = flag_set_if(list->children[i + n], c->src);
        if (!id || !ident_eq(id, first, c->src)) break;
        n++;
    }
    if (n < 2) return 1;
    if (!rule_enabled("flag-soup", c->opts)) return n;
    char msg[320];
    snprintf(msg, sizeof msg,
        "%d consecutive ifs each assign '%.*s' a literal — combine the conditions "
        "with `||`/`&&` and bind once: `let %.*s=if(c1||c2){a}el{b}`",
        n, first->tok_len, c->src + first->tok_start, first->tok_len, c->src + first->tok_start);
    const Node *ifn = list->children[i];
    if (lint_push(c->out, "flag-soup", msg, ifn->start, ifn->line, ifn->col,
                  LINT_WARNING, 0, ifn->start,
                  span_end_of_stmt(list->children[i + n - 1], c->src, c->src_len), NULL) < 0)
        *err = 1;
    return n;
}

/* ── Rule 3: string-concat-chain ──────────────────────────────────────── */

/* concat_call — n is a concat call; *module set when `alias.concat(...)`. */
static int concat_call(PatCtx *c, const Node *n, int *module)
{
    const Node *base = NULL;
    const Node *m = method_call(n, &base);
    if (!m || !method_is(m, "concat", c->src)) return 0;
    *module = is_alias(&c->aliases, base, c->src);
    return 1;
}

/* Operand k of a concat call (module: args; method: receiver then args). */
static int concat_operands(PatCtx *c, const Node *n, const Node **ops, int max)
{
    int module = 0, k = 0;
    if (!concat_call(c, n, &module)) return 0;
    if (!module && k < max) ops[k++] = n->children[0]->children[0];
    for (int i = 1; i < n->child_count && k < max; i++) ops[k++] = n->children[i];
    return k;
}

static int concat_depth(PatCtx *c, const Node *n)
{
    int module = 0;
    if (!concat_call(c, n, &module)) return 0;
    const Node *ops[32];
    int k = concat_operands(c, n, ops, 32);
    int best = 0;
    for (int i = 0; i < k; i++) {
        int d = concat_depth(c, ops[i]);
        if (d > best) best = d;
    }
    return 1 + best;
}

/* Flatten leaves in order; returns 0 if any leaf is not ident/str-lit. */
static int concat_leaves(PatCtx *c, const Node *n, const Node **leaves, int *count, int max)
{
    int module = 0;
    if (concat_call(c, n, &module)) {
        const Node *ops[32];
        int k = concat_operands(c, n, ops, 32);
        for (int i = 0; i < k; i++)
            if (!concat_leaves(c, ops[i], leaves, count, max)) return 0;
        return 1;
    }
    if (*count >= max) return 0;
    if (!(is_ident(n) || (n->kind == NODE_STR_LIT && n->tok_len >= 2))) return 0;
    leaves[(*count)++] = n;
    return 1;
}

static int module_path_is(const Node *mp, const char *path, const char *src);

/* 131.110: the import alias `base` names std.str, and `base` is that alias,
 * not a local of the same name (an alias reference carries no rtype). */
static int alias_is_std_str(PatCtx *c, const Node *base)
{
    if (!is_ident(base) || base->rtype) return 0;
    const Node *root = c->root;
    if (!root || root->kind != NODE_PROGRAM) return 0;
    for (int i = 0; i < root->child_count; i++) {
        const Node *imp = root->children[i];
        if (imp && imp->kind == NODE_IMPORT && imp->child_count >= 2 &&
            ident_eq(imp->children[0], base, c->src))
            return module_path_is(imp->children[1], "std.str", c->src);
    }
    return 0;
}

static int typed_str(const Node *n)
{
    return n && n->rtype && n->rtype->kind == TY_STR;
}

/* 131.110: is every part of the chain a str, by the checker's own types?
 * The interpolation rewrite equals the concat only when every operand is a
 * str (`"\(x)"` of an i64 renders it; of an array it is E4032) and every
 * call is std.str's concat.  So, with the program type-checked cleanly
 * (opts->typed):
 *   - every call in the chain is module style through an alias that resolves
 *     to std.str, and the checker typed it str.  Method style
 *     (`a.concat(b)`) never qualifies: the checker types neither its receiver
 *     nor its result (both untyped / TY_UNKNOWN, for a str and an array
 *     alike), so nothing distinguishes the two;
 *   - every identifier leaf is typed exactly str (absent or unknown never
 *     qualifies); a string literal leaf is a str by construction. */
static int concat_chain_str_typed(PatCtx *c, const Node *n)
{
    int module = 0;
    const Node *base = NULL;
    if (concat_call(c, n, &module)) {
        if (!module || !method_call(n, &base) || !alias_is_std_str(c, base) ||
            !typed_str(n))
            return 0;
        const Node *ops[32];
        int k = concat_operands(c, n, ops, 32);
        for (int i = 0; i < k; i++)
            if (!concat_chain_str_typed(c, ops[i])) return 0;
        return k > 0;
    }
    if (n->kind == NODE_STR_LIT) return 1;
    return is_ident(n) && typed_str(n);
}

/* 131.110: the rewritten program still lexes, parses and type-checks
 * cleanly (LintOptions.recheck, as 131.104).  The types above already make
 * the rewrite equal in value; this confirms it compiles. */
static int concat_fix_rechecks(PatCtx *c, int fs, int fe, const Str *rep)
{
    const LintOptions *o = c->opts;
    size_t rl = rep->buf ? (size_t)rep->len : 0;
    size_t n = (size_t)fs + rl + (size_t)(c->src_len - fe);
    char *probe = malloc(n + 1);
    if (!probe) return 0;
    memcpy(probe, c->src, (size_t)fs);
    if (rl) memcpy(probe + fs, rep->buf, rl);
    memcpy(probe + fs + rl, c->src + fe, (size_t)(c->src_len - fe));
    probe[n] = '\0';
    void *h = NULL;
    const Node *nr = o->recheck(o->recheck_ctx, probe, (int)n, &h);
    int ok = nr != NULL;
    if (o->recheck_free) o->recheck_free(o->recheck_ctx, h);
    free(probe);
    return ok;
}

int lint_concat_fix_enabled(void)
{
    if (TKC_LINT_CONCAT_FIX) return 1;
    const char *e = getenv("TKC_LINT_CONCAT_FIX");
    return e && e[0] && strcmp(e, "0") != 0;
}

static int rule_concat_chain(PatCtx *c, const Node *n)
{
    if (!n) return 0;
    int module = 0;
    if (concat_call(c, n, &module)) {
        int depth = concat_depth(c, n);
        if (depth >= 2) {
            if (!rule_enabled("string-concat-chain", c->opts)) return 0;
            int fixable = 0, fs = 0, fe = 0;
            Str rep = {0};
            /* 131.110: only a chain the checker proves all-str, with a
             * re-check to hand (AGENTS.md section 6: absent if uncertain) */
            if (lint_concat_fix_enabled() && c->opts && c->opts->typed &&
                c->opts->recheck && concat_chain_str_typed(c, n)) {
                const Node *leaves[64]; int lc = 0;
                if (concat_leaves(c, n, leaves, &lc, 64) && lc > 0) {
                    fs = subtree_min_start(n);
                    fe = scan_call_end(c->src, c->src_len, n->start);
                    if (fs > 0 && fe > fs && balanced(c->src, fs, fe)) {
                        fixable = 1;
                        str_puts(&rep, "\"");
                        for (int i = 0; i < lc; i++) {
                            const Node *l = leaves[i];
                            if (l->kind == NODE_STR_LIT)
                                str_put(&rep, c->src + l->tok_start + 1, l->tok_len - 2);
                            else {
                                str_puts(&rep, "\\(");
                                str_put(&rep, c->src + l->tok_start, l->tok_len);
                                str_puts(&rep, ")");
                            }
                        }
                        str_puts(&rep, "\"");
                        if (!concat_fix_rechecks(c, fs, fe, &rep)) fixable = 0;
                    }
                }
            }
            char msg[256];
            snprintf(msg, sizeof msg,
                "concat nested %d deep — use interpolation `\"\\(a)\\(b)\"` "
                "or a single `s.join`", depth);
            int start = subtree_min_start(n);
            const Node *lf = leftmost_leaf(n);
            int end = scan_call_end(c->src, c->src_len, n->start);
            if (!fixable) { fs = start; fe = end > start ? end : start; }
            int rc = lint_push(c->out, "string-concat-chain", msg,
                               start, lf ? lf->line : n->line, lf ? lf->col : n->col,
                               LINT_WARNING, fixable, fs, fe, fixable ? rep.buf : NULL);
            free(rep.buf);
            return rc;   /* do not re-flag the inner chain */
        }
    }
    for (int i = 0; i < n->child_count; i++)
        if (rule_concat_chain(c, n->children[i]) < 0) return -1;
    return 0;
}

/* ── Rule 4: single-use-let ───────────────────────────────────────────── */

/* Side-effect-free initialiser allowlist (conservative). */
static int is_pure_expr(const Node *n, const char *src)
{
    if (!n) return 0;
    switch (n->kind) {
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_BOOL_LIT: case NODE_IDENT:
        return 1;
    case NODE_STR_LIT:
        return !str_lit_has_interp(n, src);
    case NODE_FIELD_EXPR:
        return n->child_count == 2 &&
               (n->children[0]->kind == NODE_IDENT || n->children[0]->kind == NODE_FIELD_EXPR) &&
               is_pure_expr(n->children[0], src) && is_ident(n->children[1]);
    case NODE_INDEX_EXPR:
        return n->child_count == 2 &&
               (n->children[0]->kind == NODE_IDENT || n->children[0]->kind == NODE_FIELD_EXPR) &&
               is_pure_expr(n->children[0], src) && is_pure_expr(n->children[1], src);
    case NODE_BINARY_EXPR:
        return n->child_count == 2 &&
               is_pure_expr(n->children[0], src) && is_pure_expr(n->children[1], src);
    case NODE_UNARY_EXPR:
        return n->child_count == 1 && is_pure_expr(n->children[0], src);
    case NODE_CAST_EXPR:
        return n->child_count >= 1 && is_pure_expr(n->children[0], src);
    default:
        return 0;
    }
}

static int is_atomic_expr(const Node *n)
{
    switch (n->kind) {
    case NODE_INT_LIT: case NODE_FLOAT_LIT: case NODE_BOOL_LIT: case NODE_STR_LIT:
    case NODE_IDENT: case NODE_FIELD_EXPR: case NODE_INDEX_EXPR:
        return 1;
    default:
        return 0;
    }
}

/* Is the whole slice one parenthesised group "( ... )"? */
static int fully_parenthesised(const char *src, int s, int e)
{
    if (e - s < 2 || src[s] != '(' || src[e - 1] != ')') return 0;
    return scan_call_end(src, e, s) == e;
}

/* in_full_slot — the token [s,e) fills a whole expression slot (bounded by
 * '(' ';' '{' or a plain '=' before, and ')' ';' '}' after), so an inlined
 * compound expression needs no parentheses there. */
static int in_full_slot(const char *src, int src_len, int s, int e)
{
    int a = s;
    while (a > 0 && (src[a - 1] == ' ' || src[a - 1] == '\t')) a--;
    if (a == 0) return 0;
    char pc = src[a - 1];
    int before_ok = (pc == '(' || pc == ';' || pc == '{') ||
                    (pc == '=' && a >= 2 && strchr("=!<>", src[a - 2]) == NULL);
    int b = e;
    while (b < src_len && (src[b] == ' ' || src[b] == '\t')) b++;
    int after_ok = (b >= src_len) || src[b] == ')' || src[b] == ';' || src[b] == '}' ||
                   src[b] == '\n' || src[b] == '\r';
    return before_ok && after_ok;
}

/* Does `stmt` bind or assign any identifier that `expr` mentions? */
static int stmt_writes_ident_of(const Node *stmt, const Node *expr, const char *src)
{
    if (!stmt) return 0;
    if ((stmt->kind == NODE_ASSIGN_STMT || stmt->kind == NODE_BIND_STMT ||
         stmt->kind == NODE_MUT_BIND_STMT || stmt->kind == NODE_LOOP_INIT) &&
        stmt->child_count > 0 && is_ident(stmt->children[0])) {
        const Node *id = stmt->children[0];
        if (ident_used_in(expr, src + id->tok_start, id->tok_len, src)) return 1;
    }
    for (int i = 0; i < stmt->child_count; i++)
        if (stmt_writes_ident_of(stmt->children[i], expr, src)) return 1;
    return 0;
}

/* Find the (single) reference and report whether it sits under a construct
 * that re-evaluates or rebinds (loop, closure, match, scope, spawn). */
static const Node *find_ref_ctx(const Node *n, const Node *parent, int idx,
                                const char *name, int len, const char *src,
                                int unsafe, int *unsafe_out)
{
    if (!n) return NULL;
    if (n->kind == NODE_IDENT) {
        if (!name_is(n, name, len, src)) return NULL;
        if (parent && parent->kind == NODE_FIELD_EXPR && idx == 1) return NULL;
        /* `let y=mut.x` — `mut.<compound expr>` is a parse hazard (syntax card) */
        if (parent && parent->kind == NODE_MUT_BIND_STMT && idx == 1) unsafe = 1;
        *unsafe_out = unsafe;
        return n;
    }
    if (n->kind == NODE_LOOP_STMT || n->kind == NODE_CLOSURE || n->kind == NODE_MATCH_STMT ||
        n->kind == NODE_SCOPE_STMT || n->kind == NODE_SPAWN_EXPR)
        unsafe = 1;
    for (int i = 0; i < n->child_count; i++) {
        const Node *r = find_ref_ctx(n->children[i], n, i, name, len, src, unsafe, unsafe_out);
        if (r) return r;
    }
    return NULL;
}

/* 127.228: is the use of `name` inside `n` the callee of a call (`name(...)`)?
 * A binding that is called holds a function value, and inlining one is not a
 * rewrite of the same program: `let sf=s.f; sf(1)` becomes `s.f(1)`, a method
 * call (E4036); `let q=a.get(1); q(3)` becomes `a.get(1)(3)`. */
static int ref_is_callee(const Node *n, const char *name, int len, const char *src)
{
    if (!n) return 0;
    if (n->kind == NODE_CALL_EXPR && n->child_count > 0 && n->children[0] &&
        n->children[0]->kind == NODE_IDENT && name_is(n->children[0], name, len, src))
        return 1;
    for (int i = 0; i < n->child_count; i++)
        if (ref_is_callee(n->children[i], name, len, src)) return 1;
    return 0;
}

/* 127.228: does the initialiser name a function value the linter can see
 * without types -- a function this file declares, or a parameter of `fn`
 * declared with a function type?  `let h=g; ...h...` is then a deliberate
 * copy of a function value, and the fix is left absent (AGENTS §6). */
static int init_is_fn_value(const PatCtx *c, const Node *init, const Node *fn)
{
    if (!init || init->kind != NODE_IDENT) return 0;
    const char *nm = c->src + init->tok_start;
    int nl = init->tok_len;
    if (program_declares_fn(c->root, nm, nl, c->src)) return 1;
    for (int i = 0; fn && i < fn->child_count; i++) {
        const Node *p = fn->children[i];
        if (p && p->kind == NODE_PARAM && p->child_count >= 2 &&
            name_is(p->children[0], nm, nl, c->src) && p->children[1] &&
            p->children[1]->kind == NODE_FUNC_TYPE)
            return 1;
    }
    return 0;
}

/* ── 131.59: the inline must be the same typed value ─────────────────── */

/* The parent of `n` inside `r` (NULL when `n` is not below `r`). */
static const Node *lint_parent_of(const Node *r, const Node *n)
{
    if (!r) return NULL;
    for (int i = 0; i < r->child_count; i++) {
        if (r->children[i] == n) return r;
        const Node *p = lint_parent_of(r->children[i], n);
        if (p) return p;
    }
    return NULL;
}

/* A numeric literal, or a unary minus over one (the grammar has no negative
 * literal).  1 = integer, 2 = float, 0 = neither. */
static int numeric_literal_kind(const Node *n)
{
    if (!n) return 0;
    if (n->kind == NODE_UNARY_EXPR && n->child_count == 1) n = n->children[0];
    if (!n) return 0;
    if (n->kind == NODE_INT_LIT) return 1;
    if (n->kind == NODE_FLOAT_LIT) return 2;
    return 0;
}

/*
 * inline_keeps_type (131.59) — may `use` (the binding's one reference, inside
 * `next`) be replaced by the initialiser text without changing what the
 * program means?  A `let` is not type-transparent in toke, so the AST alone
 * cannot answer this; the checker's types can.  D-WEB-0006v87:
 * `let count=segs.len` binds an UNTYPED local (bind_init_type adopts no
 * scalar field type), so `right>=count` compared i64 with i64, while the
 * inlined `right>=segs.len` compares i64 with the u64 `.len` — E4031 before
 * 127.53's widening, and on arithmetic a different program still:
 * `let n=a.len; n-4` is -1 where `a.len-4` is RT002 (u64 overflow).
 *
 * Offered only when every one of these holds (AGENTS.md §6 — absent if
 * uncertain):
 *   - the caller type-checked the AST cleanly (opts->typed), so rtype is the
 *     checker's own answer and not a stale or partial one;
 *   - the `let` has no annotation: `let c:i64=a.len` states a type the
 *     expression does not have, and inlining deletes the statement of it;
 *   - the type the checker gave the USE (the binding's type) is identical to
 *     the type it gave the INITIALISER, strictly: TY_UNKNOWN at any depth is
 *     never identical (tk_type_identical).  Expression types are computed
 *     bottom-up, so an identical non-literal expression stays identical in
 *     the new slot;
 *   - except a numeric LITERAL, whose type is decided by its slot, not by
 *     itself: beside a u64 the checker narrows it (lit_narrows) and the
 *     emitter lowers the operation unsigned (binop_is_unsigned), while the
 *     i64 binding it replaces widened the other side instead —
 *     `let k=5; a.len-k` is -2, `a.len-5` is RT002.  A literal is inlined
 *     only as an operand of a binary operator whose other operand the
 *     checker typed i64 (f64 for a float literal), the one slot where the
 *     literal and the binding are provably the same value, or as the whole
 *     value of `<` from a function whose return type is spelled exactly
 *     `i64` (`f64`); every other slot (array/map literal element, argument,
 *     if/mt arm, unary, any other return type) keeps the binding.
 */
/* Is `fn`'s declared return type spelled exactly `want` (a plain scalar)? */
static int fn_returns_plain(const PatCtx *c, const Node *fn, const char *want)
{
    for (int i = 0; fn && i < fn->child_count; i++) {
        const Node *rs = fn->children[i];
        if (!rs || rs->kind != NODE_RETURN_SPEC) continue;
        if (rs->child_count != 1 || !rs->children[0]) return 0;
        const Node *t = rs->children[0];
        if (t->kind != NODE_TYPE_EXPR || t->child_count != 0) return 0;
        int wl = (int)strlen(want);
        return t->tok_len == wl && memcmp(c->src + t->tok_start, want, (size_t)wl) == 0;
    }
    return 0;
}

static int inline_keeps_type(const PatCtx *c, const Node *decl, const Node *init,
                             const Node *next, const Node *use, const Node *fn)
{
    if (!c->opts || !c->opts->typed) return 0;
    if (decl->child_count != 2) return 0;                 /* annotated */
    /* The checker does not infer every position: a method-call receiver
     * (`w.contains(c)`) carries no rtype.  A string literal without
     * interpolation (is_pure_expr) and a bool literal are typed by their
     * token alone -- bind_init_type gives the immutable binding exactly str
     * / bool, and no slot re-types them -- so for those the binding's type
     * is the initialiser's even where the use was not inferred. */
    const Type *ut = use->rtype;
    if (!ut && init->rtype &&
        ((init->kind == NODE_STR_LIT && init->rtype->kind == TY_STR) ||
         (init->kind == NODE_BOOL_LIT && init->rtype->kind == TY_BOOL)))
        ut = init->rtype;
    if (!ut || !tk_type_identical(ut, init->rtype)) return 0;
    int lk = numeric_literal_kind(init);
    if (lk) {
        TypeKind want = lk == 1 ? TY_I64 : TY_F64;
        if (ut->kind != want) return 0;
        const Node *p = lint_parent_of(next, use);
        if (!p) return 0;
        /* `<k` from a function declared exactly i64 (f64): the literal is
         * judged against that same type the binding was. */
        if (p->kind == NODE_RETURN_STMT && p->child_count == 1)
            return fn_returns_plain(c, fn, lk == 1 ? "i64" : "f64");
        if (p->kind != NODE_BINARY_EXPR || p->child_count != 2) return 0;
        const Node *sib = p->children[0] == use ? p->children[1] : p->children[0];
        if (!sib || !sib->rtype || sib->rtype->kind != want) return 0;
    }
    return 1;
}

static int check_single_use_let(PatCtx *c, const Node *list, int i, const Node *fn)
{
    const Node *decl = list->children[i];
    if (decl->child_count < 2 || !is_ident(decl->children[0])) return 0;
    if (i + 1 >= list->child_count) return 0;
    const Node *next = list->children[i + 1];
    if (!next) return 0;
    const Node *id = decl->children[0];
    const Node *init = decl->children[decl->child_count - 1];
    const char *name = c->src + id->tok_start;
    int len = id->tok_len;

    /* Census over the whole function, excluding the declaration itself. */
    RefCount all = {0};
    count_refs(fn, NULL, 0, name, len, c->src, decl, &all);
    if (all.binds != 0 || all.writes != 0) return 0;          /* shadowed / rebound */
    if (all.ast_refs + all.interp_refs != 1) return 0;
    if (ident_used_in(init, name, len, c->src)) return 0;     /* self-reference */

    RefCount nxt = {0};
    count_refs(next, list, i + 1, name, len, c->src, NULL, &nxt);
    if (nxt.ast_refs + nxt.interp_refs != 1) return 0;        /* use is not in the next stmt */
    if (!rule_enabled("single-use-let", c->opts)) return 0;

    /* Fixability: pure initialiser, AST-visible use, safe context. */
    int fixable = 0, fs = 0, fe = 0;
    Str rep = {0};
    if (nxt.ast_refs == 1 && is_pure_expr(init, c->src) &&
        !stmt_writes_ident_of(next, init, c->src) &&
        !ref_is_callee(next, name, len, c->src) &&        /* 127.228 */
        !init_is_fn_value(c, init, fn)) {
        int unsafe = 0;
        const Node *use = find_ref_ctx(next, list, i + 1, name, len, c->src, 0, &unsafe);
        if (use && !unsafe && inline_keeps_type(c, decl, init, next, use, fn)) {   /* 131.59 */
            /* initialiser text: after '=' up to the statement terminator */
            int k = id->tok_start + id->tok_len;
            while (k < c->src_len && src_is_space(c->src[k])) k++;
            if (k < c->src_len && c->src[k] == '=') {
                k++;
                while (k < c->src_len && src_is_space(c->src[k])) k++;
                int e = k, depth = 0;
                while (e < c->src_len) {
                    char ch = c->src[e];
                    if (ch == '"') {
                        int q = e + 1;   /* first byte inside the string */
                        while (q < c->src_len && c->src[q] != '"') { if (c->src[q] == '\\') q++; q++; }
                        e = q + 1;
                        continue;
                    }
                    if (ch == '(' || ch == '{') depth++;
                    else if (ch == ')' || ch == '}') { if (depth == 0) break; depth--; }
                    else if (ch == ';' && depth == 0) break;
                    e++;
                }
                while (e > k && src_is_space(c->src[e - 1])) e--;
                int stmt_s = span_start_of_stmt(decl, c->src);
                int stmt_e = span_end_of_stmt(decl, c->src, c->src_len);
                int use_e  = use->tok_start + use->tok_len;
                if (e > k && stmt_e <= use->tok_start && balanced(c->src, k, e)) {
                    int wrap = !is_atomic_expr(init) && !fully_parenthesised(c->src, k, e) &&
                               !in_full_slot(c->src, c->src_len, use->tok_start, use_e);
                    str_put(&rep, c->src + stmt_e, use->tok_start - stmt_e);
                    if (wrap) str_puts(&rep, "(");
                    str_put(&rep, c->src + k, e - k);
                    if (wrap) str_puts(&rep, ")");
                    fixable = 1; fs = stmt_s; fe = use_e;
                }
            }
        }
    }
    char msg[256];
    snprintf(msg, sizeof msg,
        "binding '%.*s' is used once, in the next statement — inline it", len, name);
    if (!fixable) { fs = decl->start; fe = span_end_of_stmt(decl, c->src, c->src_len); }
    int rc = lint_push(c->out, "single-use-let", msg, decl->start, decl->line, decl->col,
                       LINT_HINT, fixable, fs, fe, fixable ? rep.buf : NULL);
    free(rep.buf);
    return rc;
}

/* ── 127.37: reference-collection handles ─────────────────────────────── */

/* `i=v:std.vec; let x=v.new(); x.push(1)` — a std.vec (set/stack/queue) handle
 * is a by-reference collection: the method-style call mutates in place and
 * the bare call is correct, so discarded-value-result must not fire on it.
 * The linter has no types, so a receiver is a handle when a binding site of
 * its name in the enclosing function is either
 *   (a) `let x=<alias>.new(...)` for an import alias — every stdlib `.new`
 *       returns a handle; value-semantic arrays/maps come from `@(...)` /
 *       `@{...}` literals or `.append/.push/.set` results, never `.new`; or
 *   (b) declared `:$vechandle` (let annotation or param) while `std.vec` is imported.
 * Anything else (literal init, unannotated param, mt/lp binding) keeps firing. */
static int module_path_is(const Node *mp, const char *path, const char *src)
{
    if (!mp || mp->kind != NODE_MODULE_PATH) return 0;
    const char *p = path;
    for (int i = 0; i < mp->child_count; i++) {
        const Node *seg = mp->children[i];
        if (!is_ident(seg)) return 0;
        if (i > 0) { if (*p != '.') return 0; p++; }
        if (strncmp(p, src + seg->tok_start, (size_t)seg->tok_len) != 0) return 0;
        p += seg->tok_len;
    }
    return *p == '\0';
}

static int program_imports(const Node *root, const char *path, const char *src)
{
    if (!root || root->kind != NODE_PROGRAM) return 0;
    for (int i = 0; i < root->child_count; i++) {
        const Node *imp = root->children[i];
        if (imp && imp->kind == NODE_IMPORT && imp->child_count >= 2 &&
            module_path_is(imp->children[1], path, src)) return 1;
    }
    return 0;
}

/* init is `<alias>.new(...)` with alias an import alias */
static int is_handle_ctor(PatCtx *c, const Node *init)
{
    const Node *base = NULL;
    const Node *m = method_call(init, &base);
    return m && method_is(m, "new", c->src) && is_alias(&c->aliases, base, c->src);
}

/* `$vechandle` type reference (not `mod.$vechandle`) with std.vec imported.
 * 127.251: std.vec's handle type was `Vec`; the .tki now spells it
 * `vechandle` (a lower-cased `vec` would collide with the module). */
static int is_handle_type(PatCtx *c, const Node *ty)
{
    return ty && ty->kind == NODE_TYPE_IDENT && ty->op != TK_DOT && ty->tok_len == 9 &&
           memcmp(c->src + ty->tok_start, "vechandle", 9) == 0 &&
           program_imports(c->root, "std.vec", c->src);
}

static int name_bound_to_handle(PatCtx *c, const Node *n, const char *name, int len)
{
    if (!n) return 0;
    if ((n->kind == NODE_BIND_STMT || n->kind == NODE_MUT_BIND_STMT) &&
        n->child_count >= 2 && name_is(n->children[0], name, len, c->src)) {
        if (is_handle_ctor(c, n->children[n->child_count - 1])) return 1;
        if (n->child_count >= 3 && is_handle_type(c, n->children[1])) return 1;
    }
    if (n->kind == NODE_PARAM && n->child_count >= 2 &&
        name_is(n->children[0], name, len, c->src) && is_handle_type(c, n->children[1]))
        return 1;
    for (int i = 0; i < n->child_count; i++)
        if (name_bound_to_handle(c, n->children[i], name, len)) return 1;
    return 0;
}

/* ── Rule 5: discarded-value-result ───────────────────────────────────── */

/* Binding sites of a name inside a function: returns count; *mut_decl set
 * to the single MUT_BIND_STMT when that is the only binding site. */
static int count_bind_sites(const Node *n, const Node *parent, int idx,
                            const char *name, int len, const char *src,
                            const Node **mut_decl)
{
    if (!n) return 0;
    int c = 0;
    if (n->kind == NODE_IDENT && name_is(n, name, len, src) && parent && idx == 0 &&
        (parent->kind == NODE_BIND_STMT || parent->kind == NODE_MUT_BIND_STMT ||
         loop_init_declares(parent) || parent->kind == NODE_PARAM)) {   /* 127.289 */
        if (parent->kind == NODE_MUT_BIND_STMT) *mut_decl = parent;
        return 1;
    }
    if (n->kind == NODE_IDENT && name_is(n, name, len, src) && parent &&
        parent->kind == NODE_MATCH_ARM && idx == 1)
        return 1;
    for (int i = 0; i < n->child_count; i++)
        c += count_bind_sites(n->children[i], n, i, name, len, src, mut_decl);
    return c;
}

static int check_discarded_result(PatCtx *c, const Node *stmt, const Node *fn, int unsafe)
{
    if (!stmt || stmt->kind != NODE_EXPR_STMT || stmt->child_count != 1) return 0;
    const Node *call = stmt->children[0];
    const Node *base = NULL;
    const Node *m = method_call(call, &base);
    if (!m) return 0;
    if (!(method_is(m, "set", c->src) || method_is(m, "push", c->src) ||
          method_is(m, "append", c->src)))
        return 0;
    if (is_alias(&c->aliases, base, c->src)) return 0;   /* module-style: vec/set modules mutate in place */
    if (is_ident(base) &&                                 /* 127.37: method-style on a vec handle */
        name_bound_to_handle(c, fn ? fn : c->root, c->src + base->tok_start, base->tok_len))
        return 0;
    if (program_declares_fn(c->root, c->src + m->tok_start, m->tok_len, c->src)) return 0;  /* UFCS user fn */
    if (!rule_enabled("discarded-value-result", c->opts)) return 0;

    int bs = subtree_min_start(base), be = subtree_max_end(base);
    if (bs < 0 || be <= bs) return 0;
    char msg[320];
    snprintf(msg, sizeof msg,
        "result discarded — write `%.*s=%.*s.%.*s(...)` (a bare call never mutates)",
        be - bs, c->src + bs, be - bs, c->src + bs, m->tok_len, c->src + m->tok_start);

    int fixable = 0;
    char rep[128] = {0};
    if (is_ident(base) && !unsafe) {
        const Node *mut_decl = NULL;
        int sites = count_bind_sites(fn, NULL, 0, c->src + base->tok_start, base->tok_len,
                                     c->src, &mut_decl);
        if (sites == 1 && mut_decl && base->tok_len < 100) {
            fixable = 1;
            snprintf(rep, sizeof rep, "%.*s=", base->tok_len, c->src + base->tok_start);
        }
    }
    return lint_push(c->out, "discarded-value-result", msg, stmt->start, stmt->line, stmt->col,
                     LINT_ERROR, fixable, stmt->start,
                     fixable ? stmt->start : span_end_of_stmt(stmt, c->src, c->src_len),
                     fixable ? rep : NULL);
}

/* ── Rule 6: loop-rebuilds-array ──────────────────────────────────────── */

static int check_loop_rebuilds_array(PatCtx *c, const Node *lp)
{
    if (!lp || lp->kind != NODE_LOOP_STMT || lp->child_count < 2) return 0;
    const Node *init = lp->children[0];
    const Node *body = lp->children[lp->child_count - 1];
    if (!init || init->kind != NODE_LOOP_INIT || init->child_count < 1 || !is_ident(init->children[0])) return 0;
    if (!body || body->kind != NODE_STMT_LIST || body->child_count != 1) return 0;
    const Node *a = body->children[0];
    if (!a || a->kind != NODE_ASSIGN_STMT || a->child_count != 2 || !is_ident(a->children[0])) return 0;
    const Node *base = NULL;
    const Node *m = method_call(a->children[1], &base);
    if (!m || !(method_is(m, "append", c->src) || method_is(m, "push", c->src))) return 0;
    if (!ident_eq(base, a->children[0], c->src)) return 0;
    const Node *call = a->children[1];
    if (call->child_count != 2) return 0;
    const Node *iv = init->children[0];
    if (!ident_used_in(call->children[1], c->src + iv->tok_start, iv->tok_len, c->src)) return 0;
    if (ident_used_in(call->children[1], c->src + base->tok_start, base->tok_len, c->src)) return 0;
    if (!rule_enabled("loop-rebuilds-array", c->opts)) return 0;
    /* Shape of the appended value: xs.get(i) alone → a copy/slice of xs;
     * f(... xs.get(i) ...) → element transform (map); index-only → range. */
    const Node *arg = call->children[1];
    const Node *elem = NULL;
    if (arg->kind == NODE_INDEX_EXPR && arg->child_count == 2 && is_ident(arg->children[0]) &&
        name_is(arg->children[1], c->src + iv->tok_start, iv->tok_len, c->src))
        elem = arg->children[0];
    int elem_inside = 0;
    if (!elem) {
        /* look for an xs.get(i) anywhere inside the argument */
        const Node *stack[64]; int sp = 0; stack[sp++] = arg;
        while (sp > 0 && !elem_inside) {
            const Node *t = stack[--sp];
            if (t->kind == NODE_INDEX_EXPR && t->child_count == 2 && is_ident(t->children[0]) &&
                name_is(t->children[1], c->src + iv->tok_start, iv->tok_len, c->src))
                elem_inside = 1;
            for (int j = 0; j < t->child_count && sp < 64; j++) if (t->children[j]) stack[sp++] = t->children[j];
        }
    }
    char msg[320];
    if (elem)
        snprintf(msg, sizeof msg,
            "loop only copies '%.*s' elements into '%.*s' — consider `%.*s.slice(start;end)` "
            "(or `%.*s.filter(&p)` when the range is a predicate)",
            elem->tok_len, c->src + elem->tok_start, base->tok_len, c->src + base->tok_start,
            elem->tok_len, c->src + elem->tok_start, elem->tok_len, c->src + elem->tok_start);
    else if (elem_inside)
        snprintf(msg, sizeof msg,
            "loop only appends f(xs.get(i)) to '%.*s' — consider `%.*s=xs.map(&f)` when the "
            "transform is a plain per-element function",
            base->tok_len, c->src + base->tok_start, base->tok_len, c->src + base->tok_start);
    else
        snprintf(msg, sizeof msg,
            "loop only appends a value of the index to '%.*s' — consider a range + `map` "
            "when the stdlib form is confirmed (131.2)",
            base->tok_len, c->src + base->tok_start);
    return lint_push(c->out, "loop-rebuilds-array", msg, lp->start, lp->line, lp->col,
                     LINT_HINT, 0, lp->start, span_end_of_stmt(lp, c->src, c->src_len), NULL);
}

/* ── Rule 7: nested-if-ladder (131.64) ───────────────────────────────── */

/*
 * The ladder `if(a){x}el{if(b){y}el{z}}` is the chain `if(a){x}el if(b){y}el{z}`
 * written with one extra block per rung.  regen/quality_rubric.md counts an
 * el-if chain as ONE nesting level (> 3 flags, > 4 fails), so every `el{if}`
 * rung costs one level of depth that the chain does not: the threshold is
 * therefore one rung, and every ladder is reported (once, at its top rung).
 *
 * The fix rewrites the top rung only — `el{ if(b)… ;}` becomes `el if(b)…` —
 * and the --fix fixpoint loop re-lints and takes the next rung, so a ladder of
 * any length converges in one invocation.  The rewrite is the identity on the
 * AST shape the emitter consumes (else = IF_STMT instead of a STMT_LIST
 * holding only that IF_STMT; the block holds no binding, so §I scoping is
 * unchanged).  It is offered only when the source is exactly that shape:
 *   - `el`, optional whitespace, `{`, optional whitespace, the inner `if`;
 *   - after the inner if, only whitespace and at most one `;` before `}`;
 *   - no `(*`, `//` or `#` anywhere in the block outside string literals
 *     (a comment would be dropped or moved);
 *   - when the outer if is in value position the inner if must have its own
 *     `el` (an expression-`if` requires one; the rewrite would not parse).
 * Otherwise the hint carries no fix.
 */

/* skip_str_lit — s[i] == '"'; offset just past the closing quote, following
 * `\(…)` interpolations (which may hold string literals of their own), or -1. */
static int skip_interp(const char *s, int len, int i);
static int skip_str_lit(const char *s, int len, int i)
{
    i++;
    while (i < len) {
        char ch = s[i];
        if (ch == '"') return i + 1;
        if (ch == '\\') {
            if (i + 1 < len && s[i + 1] == '(') {
                i = skip_interp(s, len, i + 1);
                if (i < 0) return -1;
                continue;
            }
            i += 2;
            continue;
        }
        i++;
    }
    return -1;
}

/* skip_interp — s[i] == '(' opening an interpolation; offset past its ')'. */
static int skip_interp(const char *s, int len, int i)
{
    int depth = 0;
    while (i < len) {
        char ch = s[i];
        if (ch == '"') { i = skip_str_lit(s, len, i); if (i < 0) return -1; continue; }
        if (ch == '(') depth++;
        else if (ch == ')') { if (--depth == 0) return i + 1; }
        i++;
    }
    return -1;
}

/* match_block_close — s[ob] == '{'; offset of the matching '}', -1 when
 * unbalanced, -2 when a comment opener (`(*`, `//`, `#`) appears outside a
 * string literal before it. */
static int match_block_close(const char *s, int len, int ob)
{
    int depth = 0, i = ob;
    while (i < len) {
        char ch = s[i];
        if (ch == '"') { i = skip_str_lit(s, len, i); if (i < 0) return -1; continue; }
        if (ch == '#') return -2;
        if (i + 1 < len && ((ch == '(' && s[i + 1] == '*') || (ch == '/' && s[i + 1] == '/')))
            return -2;
        if (ch == '{') depth++;
        else if (ch == '}') { if (--depth == 0) return i; }
        i++;
    }
    return -1;
}

static int is_ident_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

/* sole_inner_if — the IF_STMT that is the only statement of ifn's else block. */
static const Node *sole_inner_if(const Node *ifn)
{
    if (!ifn || ifn->kind != NODE_IF_STMT || ifn->child_count < 3) return NULL;
    const Node *els = ifn->children[2];
    if (!els || els->kind != NODE_STMT_LIST || els->child_count != 1) return NULL;
    const Node *in = els->children[0];
    return (in && in->kind == NODE_IF_STMT) ? in : NULL;
}

static int ladder_seen(const PatCtx *c, const Node *n)
{
    for (int i = 0; i < c->ladder_seen_n; i++) if (c->ladder_seen[i] == n) return 1;
    return 0;
}

static int check_nested_if_ladder(PatCtx *c, const Node *ifn, int value_pos)
{
    const Node *inner = sole_inner_if(ifn);
    if (!inner || ladder_seen(c, ifn)) return 0;

    /* Rungs of the whole ladder: follow `el if` links for free and count each
     * `el{if}`; remember every if on the way so the walk reports it once. */
    int rungs = 0;
    for (const Node *t = ifn; t && t->kind == NODE_IF_STMT && t->child_count >= 3; ) {
        const Node *nx = sole_inner_if(t);
        if (nx) rungs++;
        else if (t->children[2] && t->children[2]->kind == NODE_IF_STMT) nx = t->children[2];
        if (!nx) break;
        if (c->ladder_seen_n < 64) c->ladder_seen[c->ladder_seen_n++] = nx;
        t = nx;
    }
    if (!rule_enabled("nested-if-ladder", c->opts)) return 0;

    const char *s = c->src;
    int len = c->src_len;
    int fixable = 0;
    int span_s = ifn->start, span_e = span_end_of_stmt(ifn, c->src, c->src_len);
    char *rep = NULL;
    do {
        int k = inner->start;
        if (k <= 0 || k >= len) break;
        while (k > 0 && src_is_space(s[k - 1])) k--;
        if (k <= 0 || s[k - 1] != '{') break;
        int ob = k - 1;
        k = ob;
        while (k > 0 && src_is_space(s[k - 1])) k--;
        if (k < 2 || s[k - 1] != 'l' || s[k - 2] != 'e') break;
        int el = k - 2;
        if (el > 0 && is_ident_char(s[el - 1])) break;
        int cb = match_block_close(s, len, ob);
        if (cb < 0) break;
        int te = cb;
        while (te > inner->start && src_is_space(s[te - 1])) te--;
        if (te > inner->start && s[te - 1] == ';') te--;
        while (te > inner->start && src_is_space(s[te - 1])) te--;
        if (te <= inner->start || s[te - 1] != '}') break;
        if (!balanced(s, inner->start, te)) break;
        if (value_pos && inner->child_count < 3) break;
        int ilen = te - inner->start;
        rep = malloc((size_t)ilen + 4);
        if (!rep) return -1;
        memcpy(rep, "el ", 3);
        memcpy(rep + 3, s + inner->start, (size_t)ilen);
        rep[ilen + 3] = '\0';
        span_s = el; span_e = cb + 1; fixable = 1;
    } while (0);

    char msg[320];
    snprintf(msg, sizeof msg,
        "else block holds only another if (%d nested rung%s) — write `el if(...)`: an "
        "el-if chain is one nesting level, each `el{if}` rung adds one "
        "(quality_rubric: depth > 3 flags, > 4 fails)",
        rungs, rungs == 1 ? "" : "s");
    int rc = lint_push(c->out, "nested-if-ladder", msg, inner->start, inner->line, inner->col,
                       LINT_HINT, fixable, span_s, span_e, rep);
    free(rep);
    return rc;
}

/* ── Pattern walker ───────────────────────────────────────────────────── */

/* 127.33 — value position.  The parser yields the same NODE_IF_STMT for the
 * statement form and the A1 expression form (parse_if_core); what makes an
 * `if`/`mt` an expression is its PARENT: a bind/assign/return/call-arg/match-arm
 * (anything but a STMT_LIST), or being the tail of another value branch.  The
 * emitter's block_tail_expr defines the branch value as the block's last
 * statement when that is an EXPR_STMT (or a nested if/mt).  `value_pos` is 1
 * when `n` is such an expression-position if/mt, or a STMT_LIST that is one of
 * its branches — the tail EXPR_STMT of that list is consumed, not discarded.
 * Statement-form `if(c){xs.append(v)};` and non-tail bare calls keep firing. */
static int is_value_expr_kind(const Node *n)
{
    return n && (n->kind == NODE_IF_STMT || n->kind == NODE_MATCH_STMT);
}

static int pattern_walk(PatCtx *c, const Node *n, const Node *fn, int unsafe, int value_pos)
{
    if (!n) return 0;
    if (n->kind == NODE_FUNC_DECL) { fn = n; unsafe = 0; }
    if (n->kind == NODE_CLOSURE || n->kind == NODE_SCOPE_STMT || n->kind == NODE_SPAWN_EXPR)
        unsafe = 1;
    if (n->kind == NODE_IF_STMT && fn && check_nested_if_ladder(c, n, value_pos) < 0)
        return -1;

    if (n->kind == NODE_STMT_LIST && fn) {
        const Node *body = func_body(fn);
        for (int i = 0; i < n->child_count; i++) {
            const Node *s = n->children[i];
            if (!s) continue;
            switch (s->kind) {
            case NODE_MUT_BIND_STMT:
                if (check_mut_flag_if(c, n, i, body) < 0) return -1;
                break;
            case NODE_BIND_STMT:
                if (check_single_use_let(c, n, i, fn) < 0) return -1;
                break;
            case NODE_IF_STMT: {
                int err = 0;
                int consumed = check_flag_soup(c, n, i, &err);
                if (err) return -1;
                if (consumed > 1) i += consumed - 1;
                break;
            }
            case NODE_EXPR_STMT:
                if (value_pos && i == n->child_count - 1) break;   /* 127.33: branch value */
                if (check_discarded_result(c, s, fn, unsafe) < 0) return -1;
                break;
            case NODE_LOOP_STMT:
                if (check_loop_rebuilds_array(c, s) < 0) return -1;
                break;
            default:
                break;
            }
        }
    }
    for (int i = 0; i < n->child_count; i++) {
        const Node *ch = n->children[i];
        int child_vp;
        if (n->kind == NODE_IF_STMT)
            child_vp = (i >= 1) ? value_pos : 0;            /* branches + `el if` chain inherit */
        else if (n->kind == NODE_STMT_LIST)
            child_vp = (value_pos && i == n->child_count - 1 && is_value_expr_kind(ch)); /* tail if/mt */
        else
            child_vp = is_value_expr_kind(ch);              /* if/mt under a non-list parent = expression */
        if (pattern_walk(c, ch, fn, unsafe, child_vp) < 0) return -1;
    }
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

int tkc_lint(const Node *ast, const char *src, int src_len,
             const LintOptions *opts, LintResult *out)
{
    if (!ast || !src || !out) return -1;

    out->items = NULL;
    out->count = 0;
    out->cap   = 0;

    if (rule_unreachable_code(ast, src, opts, out) < 0) return -1;
    if (rule_empty_fn_body(ast, src, opts, out) < 0)    return -1;
    if (rule_unused_import(ast, src, src_len, opts, out) < 0) return -1;
    if (rule_redundant_bind(ast, src, src_len, opts, out) < 0) return -1;
    if (rule_unused_let(ast, src, src_len, opts, out) < 0)    return -1;
    if (rule_mutable_never_mutated(ast, src, src_len, opts, out) < 0) return -1;

    PatCtx c;
    memset(&c, 0, sizeof c);
    c.root = ast; c.src = src; c.src_len = src_len; c.opts = opts; c.out = out;
    collect_aliases(ast, &c.aliases);
    if (pattern_walk(&c, ast, NULL, 0, 0) < 0) return -1;
    if (rule_concat_chain(&c, ast) < 0) return -1;

    return 0;
}

void lint_result_free(LintResult *r)
{
    if (r) {
        for (int i = 0; i < r->count; i++) {
            free(r->items[i].message);
            free(r->items[i].replacement);
        }
        free(r->items);
        r->items = NULL;
        r->count = 0;
        r->cap   = 0;
    }
}

const char *lint_severity_str(int severity)
{
    switch (severity) {
    case LINT_HINT:    return "hint";
    case LINT_ERROR:   return "error";
    default:           return "warning";
    }
}

static void json_escape_str(FILE *out, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out);  break;
        case '\r': fputs("\\r", out);  break;
        case '\t': fputs("\\t", out);  break;
        default:
            if (c < 0x20) fprintf(out, "\\u%04x", c);
            else fputc(c, out);
        }
    }
}

void lint_emit_json(FILE *out, const LintDiag *d, const char *file, int seq)
{
    fprintf(out, "{\"schema_version\":\"1.0\",\"diagnostic_id\":\"L%04d\","
                 "\"code\":\"%s\",\"rule\":\"%s\",\"severity\":\"%s\",\"stage\":\"lint\","
                 "\"message\":\"",
            seq, d->rule_id, d->rule_id, lint_severity_str(d->severity));
    json_escape_str(out, d->message);
    fputs("\",\"file\":\"", out);
    json_escape_str(out, file ? file : "");
    fprintf(out, "\",\"pos\":{\"offset\":%d,\"line\":%d,\"col\":%d},"
                 "\"span\":{\"start\":%d,\"end\":%d}",
            d->offset, d->line, d->col, d->span_start, d->span_end);
    if (d->fixable) {
        fprintf(out, ",\"fix\":{\"span\":{\"start\":%d,\"end\":%d},\"replacement\":\"",
                d->span_start, d->span_end);
        json_escape_str(out, d->replacement ? d->replacement : "");
        fputs("\"}", out);
    }
    fputs("}\n", out);
}
