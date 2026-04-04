#ifndef TK_COMPANION_H
#define TK_COMPANION_H

/*
 * companion.h — Companion file (.tkc.md) skeleton generator for the toke
 *               reference compiler.
 *
 * Generates a deterministic markdown skeleton from AST metadata: module name,
 * function signatures, type declarations, field lists, and control flow
 * structure.  Prose fields are left as <!-- TODO --> placeholders for an LLM
 * or human to fill in.
 *
 * Story: 11.5.2  Branch: feature/companion-generation
 */

#include "parser.h"
#include <stdio.h>

/*
 * emit_companion — Write a .tkc.md companion file skeleton to the given
 *                  output stream.
 *
 * out         : output FILE* (stdout or a file opened by the caller)
 * source_path : path to the source .tk file (used for source_file field)
 * source      : full source buffer content (used for SHA-256 and token text)
 * source_len  : length of source in bytes
 * ast         : root NODE_PROGRAM node from the parser
 */
void emit_companion(FILE *out, const char *source_path, const char *source,
                    long source_len, const Node *ast);

/*
 * verify_companion — Read a .tkc.md companion file, extract source_file and
 *                    source_hash from the YAML frontmatter, compute SHA-256
 *                    of the referenced source file, and compare.
 *
 * companion_path : path to the .tkc.md file
 *
 * Returns: 0 = match, 1 = mismatch, 2 = error (file not found, bad format)
 */
int verify_companion(const char *companion_path);

/*
 * companion_diff — Compare a .tk source file's current AST against an
 *                  existing .tkc.md companion file and report structural
 *                  divergences.
 *
 * source_path : path to the .tk source file
 * source      : full source buffer content
 * source_len  : length of source in bytes
 * ast         : root NODE_PROGRAM node from the parser
 * companion_path : path to the existing .tkc.md companion file
 *
 * Reports to stdout:
 *   NEW      — functions/types in source but missing from companion
 *   REMOVED  — functions/types in companion but missing from source
 *   CHANGED  — signature differences (parameter count/types)
 *   HASH     — source_hash mismatch
 *
 * Returns: 0 = no divergences, 1 = divergences found, 2 = error
 *
 * Story: 11.5.4
 */
int companion_diff(const char *source_path, const char *source,
                   long source_len, const Node *ast,
                   const char *companion_path);

#endif /* TK_COMPANION_H */
