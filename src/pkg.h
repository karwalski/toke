#ifndef TK_PKG_H
#define TK_PKG_H

/*
 * pkg.h — package manifest parser, MVS resolver, and lock file I/O.
 *
 * Implements story 76.1.3a: Minimum Version Selection for toke's package
 * system as defined in ADR-0004-packages.md.
 *
 * This module is standalone — it does not depend on the rest of the compiler
 * and is not linked into tkc yet.
 */

#include <stdbool.h>

/* ── Limits ────────────────────────────────────────────────────────────────── */

#define PKG_MAX_NAME       64   /* max chars in a package name              */
#define PKG_MAX_VERSION    32   /* max chars in a version string            */
#define PKG_MAX_DEPS       64   /* max dependencies per manifest            */
#define PKG_MAX_LINE      512   /* max line length in pkg.toml / pkg.lock   */
#define PKG_MAX_COMMIT     65   /* 64 hex digits (SHA-256 git id) + NUL     */
#define PKG_MAX_HASH       72   /* "sha256:" + 64 hex digits + NUL          */

/* ── SemVer ────────────────────────────────────────────────────────────────── */

typedef struct {
    int major;
    int minor;
    int patch;
} PkgVersion;

/* Parse "1.2.3" into a PkgVersion.  Returns false on malformed input. */
bool pkg_version_parse(const char *s, PkgVersion *out);

/* Compare two versions.  Returns <0, 0, or >0. */
int pkg_version_cmp(PkgVersion a, PkgVersion b);

/* Format a version into buf (must hold at least PKG_MAX_VERSION bytes). */
void pkg_version_fmt(PkgVersion v, char *buf, int bufsz);

/* ── Dependency constraint ─────────────────────────────────────────────────── */

typedef struct {
    char       name[PKG_MAX_NAME];
    PkgVersion minimum;          /* from >=x.y.z constraint */
} PkgDep;

/* ── Manifest (pkg.toml) ──────────────────────────────────────────────────── */

typedef struct {
    char       name[PKG_MAX_NAME];
    PkgVersion version;
    PkgDep     deps[PKG_MAX_DEPS];
    int        dep_count;
} PkgManifest;

/*
 * Parse a pkg.toml file into a PkgManifest.
 * Returns 0 on success, -1 on error (details in errbuf if non-NULL).
 */
int pkg_manifest_parse(const char *path, PkgManifest *out,
                       char *errbuf, int errbufsz);

/*
 * Parse pkg.toml from an in-memory string (for testing).
 */
int pkg_manifest_parse_str(const char *toml, PkgManifest *out,
                           char *errbuf, int errbufsz);

/* ── Resolved dependency ───────────────────────────────────────────────────── */

typedef struct {
    char       name[PKG_MAX_NAME];
    PkgVersion version;
    char       commit[PKG_MAX_COMMIT];  /* "" = not pinned yet (122.10)  */
    char       hash[PKG_MAX_HASH];      /* "" = not pinned yet (122.10)  */
} PkgResolved;

typedef struct {
    PkgResolved entries[PKG_MAX_DEPS];
    int         count;
} PkgLock;

/* ── MVS resolver ──────────────────────────────────────────────────────────── */

/*
 * Given an array of dependency constraints (possibly from multiple manifests),
 * resolve each unique package to its minimum satisfying version.
 *
 * For each package name, the resolved version is the maximum of all minimum
 * version requirements — i.e. the smallest version that satisfies every
 * constraint.
 *
 * Returns 0 on success, -1 on error.
 */
int pkg_resolve(const PkgDep *deps, int dep_count, PkgLock *out);

/* ── Lock file I/O ─────────────────────────────────────────────────────────── */

/*
 * Lock format 2 (story 122.10, ADR-0004 "Supply-chain security"):
 *
 *   lock-version = "2"
 *   [resolved]  name = "x.y.z"
 *   [commit]    name = "<40 or 64 lower-case hex>"
 *   [hash]      name = "sha256:<64 lower-case hex>"
 *
 * [commit]/[hash] entries must name a package already in [resolved].
 */

/* Write pkg.lock.  Returns -1, writing nothing, if any entry has an invalid
 * name or a non-empty commit/hash that is malformed. */
int pkg_lock_write(const char *path, const PkgLock *lock);

/* Read pkg.lock.  Returns 0 on success, -1 on error or missing file.  A
 * malformed name, commit or hash, a duplicate entry, or a pin for a package
 * not in [resolved] is an error. */
int pkg_lock_read(const char *path, PkgLock *out);

/* ── Supply-chain checks (story 122.10) ────────────────────────────────────── */

/* Package name: [a-z][a-z0-9]{0,62}, and not std/pkg/toke/tkc. */
bool pkg_name_valid(const char *name);

/* Full git object id: exactly 40 or 64 lower-case hex digits, not all zero. */
bool pkg_commit_valid(const char *commit);

/* Content hash: "sha256:" + exactly 64 lower-case hex digits. */
bool pkg_hash_valid(const char *hash);

/*
 * Refuse a lock that fetch/build cannot trust: every entry must carry a
 * valid commit AND hash.  Returns 0 if all are pinned, else -1 with the
 * first offending package named in errbuf.
 */
int pkg_lock_check_pinned(const PkgLock *lock, char *errbuf, int errbufsz);

/*
 * Compare what a fetch produced (commit it checked out, tree hash it
 * computed) with the lock entry.  Exact match of both required.  Returns 0
 * on match, else -1 with expected/actual in errbuf.
 */
int pkg_lock_verify(const PkgResolved *want, const char *got_commit,
                    const char *got_hash, char *errbuf, int errbufsz);

#endif /* TK_PKG_H */
