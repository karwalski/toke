#ifndef TKC_PYVIEW_H
#define TKC_PYVIEW_H

/*
 * pyview.h — `tkc --python-view`: a read-only, Python-like rendering of a
 * parsed toke program, for reviewers who read Python and not toke.
 *
 * Display only.  The output is NOT executable Python and NOT round-trippable
 * back to toke; toke semantics apply to everything it shows.  The rendering
 * rules and what is guaranteed are in docs/decisions/96.7-python-view.md.
 *
 * Story: 96.7
 */

#include <stdio.h>
#include "parser.h"

/*
 * tkc_python_view — render the AST rooted at `root` (a NODE_PROGRAM).
 *
 * src      : the original source text (token text is read from it)
 * name     : the file name shown in the header (the caller passes the
 *            basename, so the output does not depend on the working directory)
 *
 * Returns a malloc'd, NUL-terminated string the caller frees, or NULL on
 * allocation failure.  The output is a pure function of (root, src, name).
 */
char *tkc_python_view(const Node *root, const char *src, const char *name);

#endif /* TKC_PYVIEW_H */
