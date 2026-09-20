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
 *
 * Pattern rules (131.9, docs/lint-rules-v1.md "Pattern rules"):
 *   mut-flag-if            — `let x=mut.<lit>;` written only by the next if/el
 *   flag-soup              — ≥2 consecutive ifs each assigning x a literal
 *   string-concat-chain    — concat nested ≥ 2 deep
 *   single-use-let         — let used exactly once, in the next statement
 *   discarded-value-result — bare `x.set/push/append(...)` statement
 *                            (127.33: NOT the tail expression of an
 *                            expression-position if/el or mt arm — that
 *                            call IS the branch value, see pattern_walk)
 *                            (127.37: NOT a std.vec handle receiver —
 *                            `let x=v.new()` / `x:Vec` — by-reference,
 *                            the bare call mutates in place)
 *   loop-rebuilds-array    — lp whose body is only `acc=acc.append(f(i))`
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        if ((pk == NODE_BIND_STMT || pk == NODE_MUT_BIND_STMT ||
             pk == NODE_LOOP_INIT || pk == NODE_PARAM) && idx == 0) { rc->binds++; return; }
        if (pk == NODE_MATCH_ARM && idx == 1) { rc->binds++; return; }
        if (pk == NODE_ASSIGN_STMT && idx == 0) { rc->writes++; return; }
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

/* count_assign_to — number of NODE_ASSIGN_STMT whose LHS is (name,len). */
static int count_assign_to(const Node *node, const char *name, int len, const char *src)
{
    if (!node) return 0;
    int c = 0;
    if (node->kind == NODE_ASSIGN_STMT && node->child_count > 0 &&
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
 * AST GAP. `parse_type_expr` lowers a qualified type `alias.$name` to a
 * single NODE_TYPE_IDENT whose token is *name*, marked `op == TK_DOT`
 * (src/parser.c:413).  The three tokens it consumed for `alias`, `.` and
 * `$` are dropped on the floor, so the alias has no node anywhere in the
 * tree and `ident_used_in` — which compares node tokens — can never see it.
 *
 * That gap made `unused-import` advise deleting an import that the program
 * genuinely depends on, and the import list is not merely a name-resolution
 * scope: it is the compiler's module manifest.  Three codegen consumers key
 * off it directly —
 *   - resolve_stdlib_deps_imports_only (src/stdlib_deps.c) picks the C
 *     runtime files to compile in, so a dropped import silently drops the
 *     module's implementation out of the link set;
 *   - register_tki_struct_types (src/llvm.c) loads stdlib/<mod>.tki to get
 *     record field -> GEP indices, so a dropped import makes field access
 *     fall back to index 0 — a silently wrong field, not a diagnostic;
 *   - prepass_load_tki / resolve_stdlib_call map the alias to a C symbol.
 * Deleting an import is therefore a semantic change to the emitted binary.
 *
 * Until the parser preserves the alias (filed separately), recover it from
 * the source text that precedes the type token: `alias . $ name`.
 */
static int qual_type_alias_is(const Node *n, const char *name, int name_len,
                              const char *src)
{
    int p;
    int end, s;

    if (!n || n->kind != NODE_TYPE_IDENT || n->op != TK_DOT) return 0;

    /* Step back over the '$' that precedes the type name. */
    p = n->tok_start - 1;
    while (p >= 0 && (src[p] == ' ' || src[p] == '\t')) p--;
    if (p < 0 || src[p] != '$') return 0;
    p--;
    while (p >= 0 && (src[p] == ' ' || src[p] == '\t')) p--;
    if (p < 0 || src[p] != '.') return 0;
    p--;
    while (p >= 0 && (src[p] == ' ' || src[p] == '\t')) p--;

    /* Scan back over the alias identifier. */
    end = p + 1;
    s = end;
    while (s > 0 && (isalnum((unsigned char)src[s - 1]) || src[s - 1] == '_'))
        s--;
    if (end - s != name_len) return 0;
    return memcmp(src + s, name, (size_t)name_len) == 0;
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
 */
static int rule_mutable_never_mutated(const Node *node, const char *src,
                                       int src_len,
                                       const LintOptions *opts, LintResult *out)
{
    if (!node) return 0;

    if (node->kind == NODE_FUNC_DECL && node->child_count > 0) {
        const Node *body = func_body(node);
        if (body) {
            for (int i = 0; i < body->child_count; i++) {
                const Node *stmt = body->children[i];
                if (!stmt || stmt->kind != NODE_MUT_BIND_STMT) continue;
                if (stmt->child_count == 0) continue;

                const Node *ident = stmt->children[0];
                if (!is_ident(ident)) continue;

                const char *name = src + ident->tok_start;
                int name_len = ident->tok_len;

                if (!has_assign_to(body, name, name_len, src) &&
                    rule_enabled("mutable-never-mutated", opts)) {
                    char msg[256];
                    snprintf(msg, sizeof msg,
                        "mutable binding '%.*s' is never reassigned; "
                        "consider removing 'mut'", name_len, name);
                    /* Locate the `mut.` token after the '=' that follows the name */
                    int fixable = 0, fs = 0, fe = 0;
                    int k = ident->tok_start + ident->tok_len;
                    while (k < src_len && src[k] == ' ') k++;
                    if (k < src_len && src[k] == '=') {
                        k++;
                        while (k < src_len && src[k] == ' ') k++;
                        if (k + 4 <= src_len && memcmp(src + k, "mut.", 4) == 0) {
                            fixable = 1; fs = k; fe = k + 4;
                        } else if (k + 4 <= src_len && memcmp(src + k, "MUT.", 4) == 0) {
                            fixable = 1; fs = k; fe = k + 4;
                        }
                    }
                    if (lint_push(out, "mutable-never-mutated", msg,
                                  stmt->start, stmt->line, stmt->col, LINT_WARNING,
                                  fixable, fs, fe, "") < 0)
                        return -1;
                }
            }
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        if (rule_mutable_never_mutated(node->children[i], src, src_len, opts, out) < 0)
            return -1;
    }
    return 0;
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
            if (lint_concat_fix_enabled()) {
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
        !stmt_writes_ident_of(next, init, c->src)) {
        int unsafe = 0;
        const Node *use = find_ref_ctx(next, list, i + 1, name, len, c->src, 0, &unsafe);
        if (use && !unsafe) {
            /* initialiser text: after '=' up to the statement terminator */
            int k = id->tok_start + id->tok_len;
            while (k < c->src_len && src_is_space(c->src[k])) k++;
            if (k < c->src_len && c->src[k] == '=') {
                k++;
                while (k < c->src_len && src_is_space(c->src[k])) k++;
                int e = k, depth = 0;
                while (e < c->src_len) {
                    char ch = c->src[e];
                    if (ch == '"') { e++; while (e < c->src_len && c->src[e] != '"') { if (c->src[e] == '\\') e++; e++; } e++; continue; }
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
 *   (b) declared `:Vec` (let annotation or param) while `std.vec` is imported.
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

/* bare `Vec` type reference (not `mod.$Vec`) with std.vec imported */
static int is_handle_type(PatCtx *c, const Node *ty)
{
    return ty && ty->kind == NODE_TYPE_IDENT && ty->op != TK_DOT && ty->tok_len == 3 &&
           memcmp(c->src + ty->tok_start, "Vec", 3) == 0 &&
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
         parent->kind == NODE_LOOP_INIT || parent->kind == NODE_PARAM)) {
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
