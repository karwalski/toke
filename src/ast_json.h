/*
 * ast_json.h — AST-to-JSON serialiser for tooling protocol.
 *
 * Walks a parsed AST and emits valid JSON to a FILE stream.
 * Downstream tools can consume the output with any JSON parser,
 * avoiding the need to duplicate tkc's lexer/parser.
 *
 * Story: 10.8.4
 */

#ifndef TK_AST_JSON_H
#define TK_AST_JSON_H

#include <stdio.h>
#include "parser.h"

/*
 * ast_dump_json — serialise the AST rooted at `root` as JSON.
 *
 * src   : the original source text (used to extract identifier/literal text)
 * out   : destination stream (typically stdout)
 *
 * The output is a single JSON object with recursive "children" arrays.
 * Each node includes: kind, line, col, offset, span, and optionally
 * "name" (identifiers) or "value" (literals).
 */
void ast_dump_json(const Node *root, const char *src, FILE *out);

#endif /* TK_AST_JSON_H */
