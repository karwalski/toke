#ifndef TK_REGEX_CHECK_H
#define TK_REGEX_CHECK_H

/*
 * regex_check.h — compile-time validation of literal regex patterns
 * (story 137.9).
 *
 * A std.str regex call (containsre, countre, findall, matches, replacere)
 * whose pattern argument is a string literal has that pattern compiled at
 * --check time by the same engine the runtime uses (src/stdlib/tk_regex.h).
 * A pattern outside the supported language is E4075, located at the
 * offending byte inside the literal.  A computed pattern is checked at run
 * time instead (RT008).
 */

#include "parser.h"
#include "names.h"

#define E4075 4075  /* regex pattern literal is not in the supported regex language */

/* Emits one E4075 per invalid literal pattern.  Returns the number emitted. */
int regex_check(const Node *ast, const char *src, int src_len, const SymbolTable *st);

#endif /* TK_REGEX_CHECK_H */
