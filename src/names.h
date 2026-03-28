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

/* ── Name resolution ─────────────────────────────────────────────────── */

typedef enum {
    DECL_FUNC,
    DECL_TYPE,
    DECL_CONST,
    DECL_PARAM,
    DECL_LET,
    DECL_MUT,
    DECL_PREDEFINED,   /* true, false, built-in types */
    DECL_IMPORT_ALIAS, /* alias introduced by an import */
} DeclKind;

typedef struct Decl {
    const char   *name;      /* interned in arena */
    int           name_len;
    DeclKind      kind;
    const Node   *def_node;  /* NULL for predefined */
    struct Decl  *next;      /* linked list within scope */
} Decl;

typedef struct Scope {
    Decl         *head;
    struct Scope *parent;
} Scope;

typedef struct {
    Scope        *module_scope;
    Arena        *arena;
} NameEnv;

/* Error codes for name resolution */
#define E3011 3011  /* identifier not declared    */
#define E3012 3012  /* identifier already declared in this scope */

/* Resolve all identifier references in the AST.
 * symtab: output of resolve_imports().
 * src:    original source buffer.
 * Returns 0 on success, -1 if any identifier could not be resolved. */
int resolve_names(const Node *ast, const char *src,
                  const SymbolTable *symtab, Arena *arena, NameEnv *out);

#endif /* TK_NAMES_H */
