#ifndef TK_PKG_H
#define TK_PKG_H

/*
 * pkg.h — package manifest parser, MVS resolver, and lock file I/O.
 *
 * Implements story 76.1.3a: Minimum Version Selection for toke's package
 * system as defined in ADR-0004-packages.md.
 *
 * This module is standalone and network-free -- it does not depend on the
 * rest of the compiler.  76.1.3b-d link it into tkc: `tkc pkg` (src/pkg_cli.c)
 * drives it, src/pkg_git.c is the only code that runs git, and the build's
 * pkg.* import path (pkg_cli.c, pkg_build_*) reads the lock through it.
 */

#include <stdbool.h>

/* ── Diagnostic codes (reserved) ───────────────────────────────────────────── */

/* 121.50 (4): the codes docs/reference/package-manifest.md publishes for
 * package resolution.  Defined here so the code registry
 * (scripts/check_error_codes.py) lists them and no compiler diagnostic takes
 * one; 76.1.3b-d emit them (src/pkg_cli.c).  The manifest used to give "no version satisfies the
 * constraints" as E9010, which the compiler already uses for "compiler limit
 * exceeded"; codes are stable, so that condition gets a new code at the end of
 * the E9xxx series -- E9025, since E9024 went to the compiler first (121.28)
 * -- and E9010 keeps its meaning.  E9011-E9014 and W9010
 * collided with nothing and keep their numbers. */
#define E9011 9011 /* Package not found: no git source and no index, or not a dependency */
#define E9012 9012 /* Malformed pkg.toml */
#define E9013 9013 /* Git clone or fetch of a dependency failed */
#define E9014 9014 /* No git tag matches the resolved version */
#define E9025 9025 /* No version of a dependency satisfies every constraint */
#define W9010 9010 /* pkg.lock is out of date relative to pkg.toml */

/* 76.1.3b-d: the package commands and the build's pkg.* import path emit the
 * codes above and these, added at the end of the E9xxx series (E9027 was the
 * last; W9010 the last W90xx). */
#define E9028 9028 /* pkg.lock is missing, unreadable or malformed */
#define E9029 9029 /* A locked package is not pinned or not in the package cache */
#define E9030 9030 /* A fetched or cached package does not match its lock entry */
#define E9031 9031 /* tkc pkg: invalid command, argument or flag */
#define E9032 9032 /* A package file could not be read or written */
#define E9033 9033 /* Circular package dependency */
#define E9034 9034 /* Two manifests give one package different sources */
#define E9035 9035 /* A package's modules could not be built */
#define W9011 9011 /* tkc pkg init: pkg.toml already exists, left unchanged */

/* ── Limits ────────────────────────────────────────────────────────────────── */

#define PKG_MAX_NAME       64   /* max chars in a package name              */
#define PKG_MAX_VERSION    32   /* max chars in a version string            */
#define PKG_MAX_DEPS       64   /* max dependencies per manifest            */
#define PKG_MAX_LINE      512   /* max line length in pkg.toml / pkg.lock   */
#define PKG_MAX_COMMIT     65   /* 64 hex digits (SHA-256 git id) + NUL     */
#define PKG_MAX_HASH       72   /* "sha256:" + 64 hex digits + NUL          */
#define PKG_MAX_URL       256   /* max chars in a source URL (121.39)       */

/* ── SemVer ────────────────────────────────────────────────────────────────── */

typedef struct {
    int major;
    int minor;
    int patch;
} PkgVersion;

/* Parse a SemVer 2.0 version ("1.2.3", "1.2.3-rc.1", "1.2.3+build.5") into
 * a PkgVersion.  Returns false on malformed input, leading zeros, or a
 * component over PKG_VERSION_MAX.  121.46: a pre-release or build suffix is
 * validated and then dropped -- it takes no part in resolution, as
 * docs/reference/package-manifest.md documents. */
bool pkg_version_parse(const char *s, PkgVersion *out);

/* 121.46: the largest MAJOR, MINOR or PATCH accepted (fits an int). */
#define PKG_VERSION_MAX 999999999

/* Compare two versions.  Returns <0, 0, or >0. */
int pkg_version_cmp(PkgVersion a, PkgVersion b);

/* Format a version into buf (must hold at least PKG_MAX_VERSION bytes). */
void pkg_version_fmt(PkgVersion v, char *buf, int bufsz);

/* ── Dependency constraint ─────────────────────────────────────────────────── */

typedef struct {
    char       name[PKG_MAX_NAME];
    PkgVersion minimum;          /* "x.y.z" or ">=x.y.z" */
    PkgVersion below;            /* 121.46: ">=x.y.z <a.b.c" -- exclusive */
    bool       has_below;
    bool       exact;            /* 121.46: bare "x.y.z" means exactly it */
    char       git[PKG_MAX_URL]; /* extended form's `git`; "" = via the
                                    index (121.39: always pkg_source_valid) */
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
 *
 * 121.39: the schema is closed.  [package] (name, version), [dependencies]
 * (name = "constraint") and [dependencies.<name>] (version, git) are the
 * only tables and keys; anything else -- a [scripts] table, a `build` key, a
 * line that is not key = "value" -- is an error, not ignored.  A package has
 * no install hooks (ADR-0004 section 4), and a key ignored today is one a
 * later version could start to honour.  A dependency may appear once; a
 * `git` URL must pass pkg_source_valid.
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
    char       source[PKG_MAX_URL];     /* "" = not pinned yet (121.39)  */
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

/* 121.39: and [source] name = "<https or ssh URL>" -- where the commit and
 * hash came from, recorded once so a later fetch never asks the index again.
 * Any other table in a lock is an error. */

/* Write pkg.lock.  Returns -1, writing nothing, if any entry has an invalid
 * name or a non-empty commit/hash/source that is malformed. */
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
 * Source URL policy (121.39, ADR-0004 section 2): an authenticated transport
 * only -- https://host/path, ssh://[user@]host[:port]/path, or the scp form
 * [user@]host:path.  Refused: http, git, file and every other scheme, local
 * paths, git's "<transport>::" helper syntax (ext:: runs a command), any
 * URL, host, user or path beginning with '-' (read by git or ssh as an
 * option), credentials in an https URL, whitespace, control characters,
 * quotes and backslashes.
 */
bool pkg_source_valid(const char *url);

/*
 * 76.1.3d: the explicit local-path override ADR-0004 section 2 allows.  When
 * on, pkg_source_valid also accepts file:///<absolute path> (no ".."
 * segment), in manifests and locks alike.  Process-global, off by default;
 * set only from an explicit command-line flag (--allow-file-source).
 */
void pkg_set_allow_file_sources(bool on);
bool pkg_allow_file_sources(void);

/*
 * Refuse a lock that fetch/build cannot trust: every entry must carry a
 * valid source, commit AND hash.  Returns 0 if all are pinned, else -1 with
 * the first offending package named in errbuf.
 */
int pkg_lock_check_pinned(const PkgLock *lock, char *errbuf, int errbufsz);

/*
 * Compare what a fetch produced (commit it checked out, tree hash it
 * computed) with the lock entry.  Exact match of both required.  Returns 0
 * on match, else -1 with expected/actual in errbuf.
 */
int pkg_lock_verify(const PkgResolved *want, const char *got_commit,
                    const char *got_hash, char *errbuf, int errbufsz);

/*
 * The canonical tree hash of ADR-0004 (121.39): "sha256:" + hex SHA-256 over
 * every regular file under dir, sorted by '/'-separated path bytes, each
 * contributing "path NUL size-in-decimal NUL bytes".  A .git entry at the
 * root is skipped; a .git anywhere else, a symlink, or any file that is not
 * regular or a directory makes the tree unhashable (-1, the path named in
 * errbuf), as does a file that cannot be read.  Directories contribute only
 * through their files.  Returns 0 and writes out.
 */
int pkg_tree_hash(const char *dir, char out[PKG_MAX_HASH],
                  char *errbuf, int errbufsz);

/*
 * THE verification entry point for a fetched or cached package (121.39):
 * the entry must be fully pinned (source, commit, hash), got_commit must
 * equal the pin, and the tree at dir is hashed HERE and must equal the pin.
 * A fetch or build hands over the directory, never a hash it computed, so
 * there is no way to verify without hashing.  Returns 0, else -1 with the
 * reason in errbuf.
 */
int pkg_verify_fetched(const PkgResolved *want, const char *got_commit,
                       const char *dir, char *errbuf, int errbufsz);

#endif /* TK_PKG_H */
