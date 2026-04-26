/*
 * stdlib_deps.h — Selective stdlib linking: dependency table and resolver.
 *
 * Story: 46.1.2 (selective stdlib linking based on imports)
 *
 * Instead of linking every stdlib .c file into every compiled binary,
 * this module maps each stdlib module name to its .c files, transitive
 * dependencies, and extra linker flags.  resolve_stdlib_deps() performs
 * the transitive closure and returns only the files/flags actually needed.
 */

#ifndef TK_STDLIB_DEPS_H
#define TK_STDLIB_DEPS_H

#include "names.h"  /* SymbolTable, ImportEntry */

/* ── StdlibModule — one row in the static dependency table ──────────── */

typedef struct {
    const char *module;      /* "str", "http", "file", etc.              */
    const char *c_files;     /* space-separated .c basenames: "str.c"    */
    const char *deps;        /* space-separated module deps: "encoding log" */
    const char *extra_flags; /* extra linker flags: "-lm", "-lz", etc.   */
} StdlibModule;

/* ── ResolvedDeps — output of resolve_stdlib_deps() ─────────────────── */

typedef struct {
    char sources[8192];     /* space-separated paths to .c files         */
    char flags[512];        /* space-separated extra linker flags        */
} ResolvedDeps;

/* ── Public API ─────────────────────────────────────────────────────── */

/*
 * resolve_stdlib_deps — Given a SymbolTable (from resolve_imports),
 * collect only the stdlib .c files and linker flags needed.
 *
 * Always includes tk_runtime.c and tk_web_glue.c.
 * Performs transitive closure over the dependency graph.
 *
 * stdlib_dir: path to the stdlib directory (e.g. from TKC_STDLIB_DIR).
 * st:         symbol table containing import entries.
 * out:        caller-supplied ResolvedDeps, filled in by this call.
 *
 * Returns 0 on success, -1 if stdlib_dir is NULL/empty.
 */
int resolve_stdlib_deps(const char *stdlib_dir, const SymbolTable *st,
                        ResolvedDeps *out);

#endif /* TK_STDLIB_DEPS_H */
