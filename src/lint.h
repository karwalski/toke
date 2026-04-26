#ifndef TK_LINT_H
#define TK_LINT_H

/*
 * lint.h — Static analysis lint rules for the toke reference compiler.
 *
 * The lint module traverses the AST after successful parse+name-resolution
 * and produces LintDiag entries for common code-quality issues.
 *
 * Story: 45.1.2  Branch: feature/compiler-lint
 */

#include "parser.h"

/* ── Diagnostic entry ─────────────────────────────────────────────────── */

typedef struct {
    const char *rule_id;    /* e.g. "unreachable-code"  */
    const char *message;    /* human-readable message    */
    int         line;       /* 1-based line              */
    int         col;        /* 1-based column            */
    int         severity;   /* 0 = hint, 1 = warning     */
    int         fixable;    /* 1 if --fix can auto-remove */
    int         span_start; /* byte offset: start of fixable region */
    int         span_end;   /* byte offset: end of fixable region   */
} LintDiag;

/* ── Result container ─────────────────────────────────────────────────── */

typedef struct {
    LintDiag *items;
    int       count;
    int       cap;
} LintResult;

/* ── Rule filtering options ───────────────────────────────────────────── */

typedef struct {
    const char **only_rules;    /* NULL = run all rules   */
    int          only_count;
    const char **ignore_rules;  /* NULL = ignore none     */
    int          ignore_count;
} LintOptions;

/* ── Public API ───────────────────────────────────────────────────────── */

/*
 * tkc_lint — run lint rules over the parsed AST.
 *
 * ast     : root NODE_PROGRAM from parse()
 * src     : original source text (for identifier comparison)
 * src_len : length of src in bytes
 * opts    : rule filtering options (may be NULL for defaults)
 * out     : caller-provided result struct (zero-initialised)
 *
 * Returns 0 on success, -1 on allocation failure.
 */
int  tkc_lint(const Node *ast, const char *src, int src_len,
              const LintOptions *opts, LintResult *out);

void lint_result_free(LintResult *r);

#endif /* TK_LINT_H */
