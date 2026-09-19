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
#include "tkc_limits.h"

/* ── Error codes ──────────────────────────────────────────────────────── */

#define E2030 2030  /* unresolved import                                 */
#define E2031 2031  /* circular import detected                          */
#define E2035 2035  /* malformed version string in import                */
#define E2036 2036  /* no compatible version found                       */
#define E2037 2037  /* version conflict between imports                  */
#define W2038 2038  /* module name normalised from wrong capitalisation   */

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
    char *version;      /* heap-allocated version string, or NULL if unversioned */
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
 *   search_paths      : array of directories to search for .tki files
 *   search_path_count : number of entries in search_paths
 *   out               : caller-supplied SymbolTable; initialised by this call
 *
 * Returns 0 if every import was resolved, -1 if one or more could not be
 * resolved (E2030) or a circular dependency was detected (E2031).
 * Diagnostics are emitted via diag_emit() for each failure.
 */
int resolve_imports(const Node *ast, const char *src,
                    const char **search_paths, int search_path_count,
                    const TkcLimits *limits,
                    SymbolTable *out);

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
    DECL_IMPORT_ALIAS,  /* alias introduced by an import */
    DECL_CLOSURE_PARAM, /* parameter of a closure expression */
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

/* ── Closure capture info (story 76.1.9b) ──────────────────────────── */

/*
 * CaptureInfo — records which variables a closure captures from enclosing
 * scopes.  One entry per NODE_CLOSURE encountered during name resolution.
 * The codegen phase (76.1.9c) uses this to build closure environments.
 *
 *   closure_node — the NODE_CLOSURE AST node this info belongs to.
 *   cap_names    — arena-allocated array of interned name pointers.
 *   cap_count    — number of captured variables.
 */
typedef struct {
    const Node   *closure_node;
    const char  **cap_names;     /* arena-allocated array */
    int           cap_count;
} CaptureInfo;

/* ── Imported .tki type layouts (story 127.66) ──────────────────────
 *
 * A type declared in an imported `.tki` interface used to reach the type
 * checker as a bare name with no structure: `seed_predefined()` registered
 * the identifier so `$ookecfg` parsed, but nothing recorded what fields the
 * type has.  resolve_type() therefore fell through to TY_UNKNOWN and the
 * E4025 field check — which requires TY_STRUCT — could not run, so
 * `cfg.logaccess` on an imported type compiled and codegen read field slot 0.
 * The layouts below carry the .tki `"kind":"type"` records across the import
 * boundary so the checker sees the same struct a local `t=` declaration
 * would give it.
 *
 *   field_types hold the *toke* type spelling from the .tki ("str", "i64",
 *   "@$str", …); types.c maps them onto Type values.
 */
typedef struct {
    const char  *name;          /* interned type name (e.g. "ookecfg")   */
    int          is_sum;        /* 1 for sum types — no field check      */
    int          field_count;
    const char **field_names;   /* arena array of interned names          */
    const char **field_types;   /* arena array of interned toke types     */
} ImportedType;

/*
 * ImportedFunc — one `"kind":"func"` export of an imported module, keyed by
 * the alias it was imported under, so `cli.cfgdefault()` can be given the
 * declared return type its .tki states (story 127.66).
 */
typedef struct {
    const char  *alias;         /* import alias (e.g. "cli")             */
    const char  *fn;            /* export name (e.g. "cfgdefault")       */
    const char  *ret;           /* declared toke return type spelling    */
} ImportedFunc;

typedef struct {
    Scope        *module_scope;
    Arena        *arena;
    /* Closure capture side-table (story 76.1.9b) */
    CaptureInfo  *captures;      /* arena-allocated array */
    int           capture_count;
    int           capture_cap;
    /* Imported .tki interface surface (story 127.66) */
    ImportedType *itypes;        /* arena-allocated array */
    int           itype_count;
    int           itype_cap;
    ImportedFunc *ifuncs;        /* arena-allocated array */
    int           ifunc_count;
    int           ifunc_cap;
} NameEnv;

/* Look up an imported .tki type layout by name; NULL when not imported. */
const ImportedType *imported_type_lookup(const NameEnv *env, const char *name);
/* Look up the declared return type of `alias.fn`; NULL when not known. */
const char *imported_func_ret(const NameEnv *env, const char *alias,
                              const char *fn);

/* Error codes for name resolution */
#define E3011 3011  /* identifier not declared    */
#define E3012 3012  /* identifier already declared in this scope */

/* Resolve all identifier references in the AST.
 * symtab: output of resolve_imports().
 * src:    original source buffer.
 * Returns 0 on success, -1 if any identifier could not be resolved. */
int resolve_names(const Node *ast, const char *src,
                  const SymbolTable *symtab, Arena *arena, NameEnv *out,
                  const char **search_paths, int search_path_count);

#endif /* TK_NAMES_H */
