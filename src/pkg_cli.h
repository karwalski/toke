#ifndef TK_PKG_CLI_H
#define TK_PKG_CLI_H

/*
 * pkg_cli.h — `tkc pkg` commands (story 76.1.3b), on the 76.1.3a resolver and lock (pkg.h) and
 * the git fetch (pkg_git.h, 76.1.3d).  Design: ADR-0004.
 *
 * Every error path emits a structured diagnostic (diag.h) with a code from
 * pkg.h; none carries a `fix`.
 */

/*
 * `tkc pkg <command> ...` -- argv[0] is the command (init, add, remove,
 * resolve, fetch, list).  Returns the process exit code: 0, 1 for an error
 * diagnostic, 3 (usage) for E9031.
 */
int pkg_cli_main(int argc, char **argv);

#endif /* TK_PKG_CLI_H */
