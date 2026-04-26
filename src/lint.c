/*
 * lint.c — Static analysis lint rules for the toke reference compiler.
 *
 * Implements AST-only lint rules that run after a successful parse.
 * Each rule walks the AST recursively (switch on node->kind, recurse
 * into children) following the same pattern as ast_json.c.
 *
 * Initial rules:
 *   unreachable-code  — statements after a return (<) in a statement list
 *   empty-fn-body     — function with an empty statement list body
 *   unused-import     — import alias never referenced in the AST
 *   redundant-bind    — `let x = x;` where both sides are the same ident
 *
 * Story: 45.1.2  Branch: feature/compiler-lint
 */

#include "lint.h"
#include <stdlib.h>
#include <string.h>

/* ── Result helpers ───────────────────────────────────────────────────── */

static int lint_push(LintResult *r, const char *rule_id, const char *message,
                     int line, int col, int severity,
                     int fixable, int span_start, int span_end)
{
    if (r->count >= r->cap) {
        int newcap = r->cap == 0 ? 16 : r->cap * 2;
        LintDiag *tmp = realloc(r->items, (size_t)newcap * sizeof(LintDiag));
        if (!tmp) return -1;
        r->items = tmp;
        r->cap   = newcap;
    }
    LintDiag *d = &r->items[r->count++];
    d->rule_id    = rule_id;
    d->message    = message;
    d->line       = line;
    d->col        = col;
    d->severity   = severity;
    d->fixable    = fixable;
    d->span_start = span_start;
    d->span_end   = span_end;
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

/* ── Helper: compare identifier text ──────────────────────────────────── */

/*
 * ident_eq — return 1 if two NODE_IDENT nodes refer to the same name
 * by comparing their source text spans.
 */
static int ident_eq(const Node *a, const Node *b, const char *src)
{
    if (!a || !b) return 0;
    if (a->kind != NODE_IDENT || b->kind != NODE_IDENT) return 0;
    if (a->tok_len != b->tok_len) return 0;
    if (a->tok_len == 0) return 0;
    return memcmp(src + a->tok_start, src + b->tok_start, (size_t)a->tok_len) == 0;
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

    /* Check field access: NODE_FIELD_EXPR child[0] could be the import alias */
    for (int i = 0; i < node->child_count; i++) {
        if (ident_used_in(node->children[i], name, name_len, src))
            return 1;
    }
    return 0;
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
                                      unreachable->line, unreachable->col, 1,
                                      0, 0, 0) < 0)
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
        /* The body is the last child if it's a STMT_LIST */
        const Node *last = node->children[node->child_count - 1];
        if (last && last->kind == NODE_STMT_LIST && last->child_count == 0) {
            if (rule_enabled("empty-fn-body", opts)) {
                if (lint_push(out, "empty-fn-body",
                              "function has an empty body",
                              node->line, node->col, 1,
                              0, 0, 0) < 0)
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

/* ── Rule: unused-import ──────────────────────────────────────────────── */

/*
 * Collect NODE_IMPORT nodes from the top-level PROGRAM children.
 * For each import, extract the alias (last child, a NODE_IDENT) and
 * check if that identifier appears anywhere else in the AST.
 *
 * NODE_IMPORT structure: children are module path segments, with the
 * last child being the alias (NODE_IDENT).  We search the entire AST
 * excluding the import node itself.
 */
/*
 * span_end_of_stmt — find the byte offset just past a statement's trailing
 * semicolon and newline.  Scans forward from the last token in the node.
 */
static int span_end_of_stmt(const Node *node, const char *src, int src_len)
{
    /* Find the rightmost byte covered by any leaf in the subtree */
    int end = node->start;
    if (node->tok_len > 0 && node->tok_start + node->tok_len > end)
        end = node->tok_start + node->tok_len;
    for (int i = 0; i < node->child_count; i++) {
        if (!node->children[i]) continue;
        int ce = span_end_of_stmt(node->children[i], src, src_len);
        if (ce > end) end = ce;
    }
    /* Advance past any trailing semicolons and whitespace up to (including) newline */
    while (end < src_len && (src[end] == ';' || src[end] == ' ' || src[end] == '\t'))
        end++;
    if (end < src_len && src[end] == '\r') end++;
    if (end < src_len && src[end] == '\n') end++;
    return end;
}

static int rule_unused_import(const Node *root, const char *src,
                              int src_len,
                              const LintOptions *opts, LintResult *out)
{
    if (!root || root->kind != NODE_PROGRAM) return 0;

    for (int i = 0; i < root->child_count; i++) {
        const Node *imp = root->children[i];
        if (!imp || imp->kind != NODE_IMPORT) continue;
        if (imp->child_count == 0) continue;

        /* The alias is the last child (NODE_IDENT) */
        const Node *alias = imp->children[imp->child_count - 1];
        if (!alias || alias->kind != NODE_IDENT || alias->tok_len == 0) continue;

        const char *name = src + alias->tok_start;
        int name_len = alias->tok_len;

        /* Search all non-import top-level declarations for usage */
        int used = 0;
        for (int j = 0; j < root->child_count; j++) {
            if (j == i) continue;  /* skip the import itself */
            const Node *decl = root->children[j];
            if (!decl) continue;
            if (decl->kind == NODE_IMPORT) continue;  /* skip other imports */
            if (ident_used_in(decl, name, name_len, src)) {
                used = 1;
                break;
            }
        }

        if (!used && rule_enabled("unused-import", opts)) {
            int sp_start = imp->start;
            int sp_end   = span_end_of_stmt(imp, src, src_len);
            if (lint_push(out, "unused-import",
                          "imported module is never used",
                          imp->line, imp->col, 1,
                          1, sp_start, sp_end) < 0)
                return -1;
        }
    }
    return 0;
}

/* ── Rule: redundant-bind ─────────────────────────────────────────────── */

/*
 * `let x = x;` — a NODE_BIND_STMT where child[0] (the name) and
 * child[1] (the initialiser) are both NODE_IDENT with the same text.
 */
static int rule_redundant_bind(const Node *node, const char *src,
                               int src_len,
                               const LintOptions *opts, LintResult *out)
{
    if (!node) return 0;

    if (node->kind == NODE_BIND_STMT && node->child_count >= 2) {
        const Node *lhs = node->children[0];
        const Node *rhs = node->children[1];
        /* child[1] might be a type annotation; the init expr could be child[2] */
        if (node->child_count >= 3) {
            rhs = node->children[node->child_count - 1];
        }
        if (ident_eq(lhs, rhs, src) && rule_enabled("redundant-bind", opts)) {
            int sp_start = node->start;
            int sp_end   = span_end_of_stmt(node, src, src_len);
            if (lint_push(out, "redundant-bind",
                          "binding assigns a variable to itself",
                          node->line, node->col, 1,
                          1, sp_start, sp_end) < 0)
                return -1;
        }
    }

    for (int i = 0; i < node->child_count; i++) {
        if (rule_redundant_bind(node->children[i], src, src_len, opts, out) < 0)
            return -1;
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

    return 0;
}

void lint_result_free(LintResult *r)
{
    if (r) {
        free(r->items);
        r->items = NULL;
        r->count = 0;
        r->cap   = 0;
    }
}
