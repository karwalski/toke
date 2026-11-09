#ifndef TK_TKIR_AST_H
#define TK_TKIR_AST_H

/*
 * tkir_ast.h -- the TEXT and AST sections of a .tkir file (format 1.2), and
 * the loader behind `tkc --from-tkir`.
 *
 * Story 76.1.6c.  The format-1.1 code section is a sketch of SSA: a LOAD
 * names no variable, an assignment stores to register 0, a float literal is
 * truncated to an integer, and match, arrays, maps, closures and string
 * interpolation are not encoded at all.  Nothing can be lowered from it that
 * behaves like the program.  So 1.2 carries the parsed program instead --
 * every AST node with its kind, operator, positions and children, plus the
 * token text those nodes' spans point into -- and `--from-tkir` rebuilds the
 * AST and hands it to the same name resolver, type checker and LLVM backend
 * a source compile uses.  There is one codegen, not two.
 *
 * Layouts: docs/architecture/binary-ir-design.md §3.8.
 */

#include <stddef.h>
#include "tkir.h"
#include "parser.h"

/*
 * tkir_ast_encode -- build the TEXT and AST section bodies for `ast`, the
 * tree parse() returned (encode it BEFORE name resolution and type checking:
 * those passes annotate the tree, and --from-tkir runs them again).
 *
 * src/slen : the source the tree was parsed from
 * toks/tc  : the lexer's tokens for src (their bytes are kept)
 * profile  : the syntax profile the source was lexed and parsed with
 * src_path : the source path, recorded for reference (may be NULL)
 *
 * text_out and ast_out must be initialised (tkir_buf_init); the caller frees
 * them.  Returns 0, or -1 with E9020 emitted.
 */
int tkir_ast_encode(const Node *ast, const char *src, size_t slen,
                    const Token *toks, int tc, Profile profile,
                    const char *src_path, TkirBuf *text_out, TkirBuf *ast_out);

/*
 * tkir_load_program -- read a .tkir file and rebuild its program.
 *
 * On success returns 0 and sets *sbuf_out (malloc'd, NUL-terminated, the
 * caller frees it) to a buffer of the original source's length holding the
 * token text at its original offsets -- every node span, line and column is
 * therefore unchanged -- *slen_out to that length, *ast_out to the rebuilt
 * NODE_PROGRAM (allocated in `arena`), and *profile_out to the recorded
 * profile.  orig_path receives the recorded source path ("" if none).
 *
 * Returns -1 with an E9021 diagnostic for every failure: unreadable file,
 * malformed header or section, a file written before format 1.2 (no AST
 * section), and any node, span, reference or count that does not validate.
 */
int tkir_load_program(const char *path, Arena *arena,
                      char **sbuf_out, size_t *slen_out, Node **ast_out,
                      Profile *profile_out, char *orig_path, size_t orig_cap);

/* Decode from memory (the loader without the file read); same contract. */
int tkir_load_program_mem(const uint8_t *buf, size_t len, Arena *arena,
                          char **sbuf_out, size_t *slen_out, Node **ast_out,
                          Profile *profile_out, char *orig_path, size_t orig_cap);

#endif /* TK_TKIR_AST_H */
