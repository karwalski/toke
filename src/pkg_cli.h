#ifndef TK_PKG_CLI_H
#define TK_PKG_CLI_H

/*
 * pkg_cli.h — `tkc pkg` commands (story 76.1.3b) and the build's pkg.*
 * import path (story 76.1.3c), on the 76.1.3a resolver and lock (pkg.h) and
 * the git fetch (pkg_git.h, 76.1.3d).  Design: ADR-0004.
 *
 * Every error path emits a structured diagnostic (diag.h) with a code from
 * pkg.h; none carries a `fix`.
 */

/*
 * `tkc pkg <command> ...` -- argv[0] is the command (init, add, remove,
 * resolve, fetch, list).  Returns the process exit code: 0, 1 for an error
 * diagnostic, 3 (usage) for E9031.  `tkc pkg --help`, `tkc pkg help
 * [<command>]` and `tkc pkg <command> --help` print usage on stdout and
 * return 0 (132.142).
 */
int pkg_cli_main(int argc, char **argv);

/* ── Build integration (76.1.3c) ─────────────────────────────────────────── */

/* Generates the interface of one package module: compile `src` with
 * --emit-interface --check, writing <module>.tki into `outdir`, with
 * `extra_sp` added to the .tki search path.  Returns 0 on success.  When
 * `quiet` is set the callee suppresses diagnostics. */
typedef int (*PkgEmitIfaceFn)(const char *src, const char *outdir,
                              const char *extra_sp, int quiet, void *ud);

/*
 * Prepare the pkg.* import path for a build run from the current directory
 * (the project root).  Reads pkg.toml and pkg.lock, refuses a lock that is
 * not fully pinned, re-verifies every locked package in the cache by hashing
 * it (ADR-0004: the cache is not trusted), then regenerates each package's
 * interfaces from source into a private temporary directory.
 *
 * Never emits an error itself: a failure is recorded and reported, at the
 * import, by the first pkg.* import resolve_imports meets (the hook below),
 * so a program with no pkg.* import is unaffected.  Only W9010 (lock out of
 * date) is emitted here.
 *
 * Returns 0 when packages are ready, -1 when a failure was recorded.
 */
int pkg_build_prepare(PkgEmitIfaceFn emit, void *ud);

/* The directory holding the generated interfaces (pkg.<name>.tki and
 * pkg.<name>.<module>.tki), to add to the .tki search path; NULL when
 * pkg_build_prepare has not succeeded.  A package module declares its own
 * import path (m=pkg.<name>; / m=pkg.<name>.<module>;). */
const char *pkg_build_tki_dir(void);

/* The package source files to link into the program (each package's
 * root-level .tk files); count in *n. */
const char *const *pkg_build_sources(int *n);

/* names.c hook (names_set_pkg_hook): 0 if package `name` may be imported,
 * else the diagnostic code with the message in msg. */
int pkg_build_import_check(const char *name, char *msg, int msgsz);

/* Does the text of any of these files contain "pkg." -- a superset test for
 * "has a pkg.* import", used to skip all package work for other builds. */
int pkg_build_sources_mention_pkg(const char *const *files, int n);

#endif /* TK_PKG_CLI_H */
