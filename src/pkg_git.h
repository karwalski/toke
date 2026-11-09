#ifndef TK_PKG_GIT_H
#define TK_PKG_GIT_H

/*
 * pkg_git.h — git-based package fetch (story 76.1.3d, ADR-0004).
 *
 * The only code in tkc that runs git.  git is started as a child process
 * with an argument vector -- never through a shell -- and with every
 * repository-locating GIT_* variable removed from its environment.  The
 * transport is restricted at git's own layer too (protocol.allow=never, then
 * https and ssh allowed; file only under the explicit override), so a source
 * that slipped past pkg_source_valid still could not use another transport.
 *
 * No network code lives in tkc itself: git does the transfer.
 */

#include <stdbool.h>
#include <stddef.h>

#include "pkg.h"

/* Results of pkg_git_fetch_tag. */
#define PKG_GIT_OK      0
#define PKG_GIT_NOTAG   1   /* the repository has no tag v<version>  (E9014) */
#define PKG_GIT_FAILED  2   /* git could not run, or the transfer failed (E9013) */

/*
 * Fetch tag `tag` (e.g. "v1.2.0") of the repository at `url` into the new
 * directory `dest` (which must not exist), check out the commit it names,
 * and remove dest/.git, leaving only the working tree.  On success writes
 * the full commit id into `commit` and returns PKG_GIT_OK.  On failure
 * returns PKG_GIT_NOTAG or PKG_GIT_FAILED with git's message in err, and
 * removes whatever it created at dest.
 */
int pkg_git_fetch_tag(const char *url, const char *tag, const char *dest,
                      char commit[PKG_MAX_COMMIT], char *err, int errsz);

/* rm -rf without following symlinks.  Returns 0, or -1 (path not removed). */
int pkg_rmtree(const char *path);

/* mkdir -p, mode 0755.  Returns 0, or -1. */
int pkg_mkdirs(const char *path);

#endif /* TK_PKG_GIT_H */
