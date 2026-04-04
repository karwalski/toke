#ifndef TKC_FMT_H
#define TKC_FMT_H

#include "parser.h"

/*
 * fmt.h — canonical formatter for toke source.
 *
 * tkc_format() walks an AST (produced by parse()) and emits deterministic,
 * canonical toke source text.  The result is a malloc'd string; the caller
 * must free() it.
 *
 * tkc_format_pretty() adds human-readable whitespace, optional type
 * annotations, and heuristic identifier expansion for readability.
 * NOT intended for round-tripping back through the compiler.
 *
 * Stories: 10.8.1, 10.8.2
 */

/* Options for pretty-printing and identifier expansion. */
typedef struct {
    int pretty;    /* add whitespace for readability */
    int expand;    /* expand abbreviated identifiers */
} FmtOptions;

/* Format an AST back to canonical toke source.
 * Returns malloc'd string, caller frees.  Returns NULL on allocation failure.
 *
 * `src` is the original source text (needed to extract token text from the
 * tok_start / tok_len fields on leaf nodes).
 */
char *tkc_format(const Node *root, const char *src);

/* Format an AST with pretty-printing and/or identifier expansion.
 * Returns malloc'd string, caller frees.  Returns NULL on allocation failure.
 */
char *tkc_format_pretty(const Node *root, const char *src, FmtOptions opts);

#endif /* TKC_FMT_H */
