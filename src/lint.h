#ifndef TK_LINT_H
#define TK_LINT_H

/*
 * lint.h — Static analysis lint rules for the toke reference compiler.
 *
 * The lint module traverses the AST after successful parse and produces
 * LintDiag entries for common code-quality issues.
 *
 * Story: 45.1.2  Branch: feature/compiler-lint
 * Story: 131.9   Pattern rules + span-replacement fixes
 */

#include <stdio.h>
#include "parser.h"

/* ── Severity levels ──────────────────────────────────────────────────── */

#define LINT_HINT    0
#define LINT_WARNING 1
#define LINT_ERROR   2

/* ── Diagnostic entry ─────────────────────────────────────────────────── */

typedef struct {
    const char *rule_id;     /* e.g. "unreachable-code"  (static string)   */
    char       *message;     /* human-readable message   (owned)           */
    int         offset;      /* byte offset of the diagnostic anchor        */
    int         line;        /* 1-based line                                */
    int         col;         /* 1-based column                              */
    int         severity;    /* LINT_HINT / LINT_WARNING / LINT_ERROR       */
    int         fixable;     /* 1 if --fix can rewrite [span_start,span_end) */
    int         span_start;  /* byte offset: start of fix region            */
    int         span_end;    /* byte offset: end of fix region (exclusive)  */
    char       *replacement; /* text that replaces the fix region (owned);  */
                             /* NULL or "" = deletion                       */
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

/* Severity name: "hint" / "warning" / "error". */
const char *lint_severity_str(int severity);

/*
 * lint_emit_json — write one diagnostic as a single-line JSON object:
 *   {"schema_version":"1.0","diagnostic_id":"L0001","code":"<rule>",
 *    "rule":"<rule>","severity":"...","stage":"lint","message":"...",
 *    "file":"...","pos":{"offset","line","col"},"span":{"start","end"},
 *    "fix":{"span":{"start","end"},"replacement":"..."}}   (fix only when fixable)
 */
void lint_emit_json(FILE *out, const LintDiag *d, const char *file, int seq);

/*
 * lint_concat_fix_enabled — 1 when the string-concat-chain → interpolation
 * rewrite is armed (compile-time TKC_LINT_CONCAT_FIX=1 or the environment
 * variable TKC_LINT_CONCAT_FIX set to a non-empty value other than "0").
 * Dormant by default until Epic 127.7 / 127.10 close.
 */
int lint_concat_fix_enabled(void);

#endif /* TK_LINT_H */
