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

/* Write resolved versions to pkg.lock in TOML format. */
int pkg_lock_write(const char *path, const PkgLock *lock);

/* Read pkg.lock.  Returns 0 on success, -1 on error or missing file. */
int pkg_lock_read(const char *path, PkgLock *out);

#endif /* TK_PKG_H */
