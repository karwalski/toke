#ifndef TK_NAMES_H
#define TK_NAMES_H

/*
 * names.h — Import resolver for the toke reference compiler.
 *
 * Walks a parsed AST, resolves every NODE_IMPORT against .tki interface
 * files on disk (or the std.* stdlib stub), detects circular imports, and
 * produces a SymbolTable consumed by the name-resolution pass.
 *
 * Error codes:
 *   E2030 — unresolved import
 *   E2031 — circular import
 *
 * Story: 1.2.3  Branch: feature/compiler-import-resolver
 */

#include "parser.h"

/* ── Error codes ──────────────────────────────────────────────────────── */

#define E2030 2030  /* unresolved import                                 */
#define E2031 2031  /* circular import detected                          */

/* ── ImportEntry ──────────────────────────────────────────────────────── */

/*
 * One entry per I= declaration found in the source file.
 *   alias_name  — NUL-terminated alias identifier (e.g. "io")
 *   module_path — NUL-terminated dotted path      (e.g. "std.io")
 *   resolved    — 1 if the module was found/stubbed, 0 if not
 */
typedef struct {
    char *alias_name;   /* heap-allocated (realloc family)  */
    char *module_path;  /* heap-allocated (realloc family)  */
    int   resolved;
} ImportEntry;

/* ── SymbolTable ──────────────────────────────────────────────────────── */

/*
 * Flat array of ImportEntry values produced by resolve_imports().
 * The table itself is heap-allocated (not arena-allocated) so it can
 * outlive the Arena used for AST nodes.
 * search_path is the directory searched for .tki files (not owned here).
 */
typedef struct {
    ImportEntry *entries;     /* heap-allocated array      */
    int          count;
    const char  *search_path; /* borrowed pointer, not freed */
} SymbolTable;

/* ── Public API ───────────────────────────────────────────────────────── */

/*
 * resolve_imports — main entry point.
 *
 *   ast         : root NODE_PROGRAM node returned by parse()
 *   src         : original source bytes (same pointer passed to parse())
 *   search_path : directory to search for <module/path>.tki files
 *   out         : caller-supplied SymbolTable; initialised by this call
 *
 * Returns 0 if every import was resolved, -1 if one or more could not be
 * resolved (E2030) or a circular dependency was detected (E2031).
 * Diagnostics are emitted via diag_emit() for each failure.
 */
int resolve_imports(const Node *ast, const char *src,
                    const char *search_path, SymbolTable *out);

/*
 * symtab_free — release all heap memory owned by a SymbolTable.
 * Does not free the SymbolTable struct itself.
 */
void symtab_free(SymbolTable *st);

#endif /* TK_NAMES_H */
