/*
 * test_pkg.c — unit tests for pkg.toml parser, MVS resolver, and lock I/O.
 *
 * Story 76.1.3a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/pkg.h"

static int tests_run    = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do {                                \
    tests_run++;                                              \
    if (!(cond)) {                                            \
        fprintf(stderr, "FAIL %s:%d: %s\n",                  \
                __FILE__, __LINE__, (msg));                    \
    } else {                                                  \
        tests_passed++;                                       \
    }                                                         \
} while (0)

/* ── SemVer tests ──────────────────────────────────────────────────────────── */

static void
test_version_parse(void)
{
    PkgVersion v;

    ASSERT(pkg_version_parse("1.2.3", &v), "parse 1.2.3");
    ASSERT(v.major == 1 && v.minor == 2 && v.patch == 3, "1.2.3 values");

    ASSERT(pkg_version_parse("0.0.0", &v), "parse 0.0.0");
    ASSERT(v.major == 0 && v.minor == 0 && v.patch == 0, "0.0.0 values");

    ASSERT(pkg_version_parse("10.20.30", &v), "parse 10.20.30");
    ASSERT(v.major == 10 && v.minor == 20 && v.patch == 30, "10.20.30 values");

    ASSERT(!pkg_version_parse("1.2", &v), "reject 1.2 (no patch)");
    ASSERT(!pkg_version_parse("abc", &v), "reject abc");
    ASSERT(!pkg_version_parse("", &v), "reject empty");
    ASSERT(!pkg_version_parse(NULL, &v), "reject NULL");
}

static void
test_version_cmp(void)
{
    PkgVersion a, b;

    pkg_version_parse("1.0.0", &a);
    pkg_version_parse("2.0.0", &b);
    ASSERT(pkg_version_cmp(a, b) < 0, "1.0.0 < 2.0.0");
    ASSERT(pkg_version_cmp(b, a) > 0, "2.0.0 > 1.0.0");

    pkg_version_parse("1.2.3", &a);
    pkg_version_parse("1.2.3", &b);
    ASSERT(pkg_version_cmp(a, b) == 0, "1.2.3 == 1.2.3");

    pkg_version_parse("1.2.3", &a);
    pkg_version_parse("1.2.4", &b);
    ASSERT(pkg_version_cmp(a, b) < 0, "1.2.3 < 1.2.4");

    pkg_version_parse("1.3.0", &a);
    pkg_version_parse("1.2.9", &b);
    ASSERT(pkg_version_cmp(a, b) > 0, "1.3.0 > 1.2.9");
}

static void
test_version_fmt(void)
{
    PkgVersion v = { .major = 3, .minor = 14, .patch = 1 };
    char buf[PKG_MAX_VERSION];
    pkg_version_fmt(v, buf, (int)sizeof(buf));
    ASSERT(strcmp(buf, "3.14.1") == 0, "format 3.14.1");
}

/* ── Manifest parser tests ─────────────────────────────────────────────────── */

static void
test_manifest_basic(void)
{
    const char *toml =
        "[package]\n"
        "name = \"myapp\"\n"
        "version = \"1.0.0\"\n"
        "\n"
        "[dependencies]\n"
        "webframework = \">=0.3.0\"\n"
        "jsonparser = \">=1.2.0\"\n";

    PkgManifest m;
    char err[256] = {0};
    int rc = pkg_manifest_parse_str(toml, &m, err, (int)sizeof(err));

    ASSERT(rc == 0, "parse basic manifest");
    ASSERT(strcmp(m.name, "myapp") == 0, "package name");
    ASSERT(m.version.major == 1 && m.version.minor == 0 &&
           m.version.patch == 0, "package version");
    ASSERT(m.dep_count == 2, "dep count == 2");

    ASSERT(strcmp(m.deps[0].name, "webframework") == 0, "dep[0] name");
    ASSERT(m.deps[0].minimum.major == 0 &&
           m.deps[0].minimum.minor == 3 &&
           m.deps[0].minimum.patch == 0, "dep[0] version");

    ASSERT(strcmp(m.deps[1].name, "jsonparser") == 0, "dep[1] name");
    ASSERT(m.deps[1].minimum.major == 1 &&
           m.deps[1].minimum.minor == 2 &&
           m.deps[1].minimum.patch == 0, "dep[1] version");
}

static void
test_manifest_comments_and_blanks(void)
{
    const char *toml =
        "# This is a comment\n"
        "\n"
        "[package]\n"
        "name = \"testpkg\"\n"
        "version = \"0.1.0\"\n"
        "\n"
        "# Some deps\n"
        "[dependencies]\n"
        "# commented-out = \">=1.0.0\"\n"
        "real = \">=2.0.0\"\n";

    PkgManifest m;
    int rc = pkg_manifest_parse_str(toml, &m, NULL, 0);

    ASSERT(rc == 0, "parse with comments");
    ASSERT(strcmp(m.name, "testpkg") == 0, "name from commented file");
    ASSERT(m.dep_count == 1, "only non-comment dep");
    ASSERT(strcmp(m.deps[0].name, "real") == 0, "dep name");
}

static void
test_manifest_no_deps(void)
{
    const char *toml =
        "[package]\n"
        "name = \"bare\"\n"
        "version = \"0.1.0\"\n"
        "\n"
        "[dependencies]\n";

    PkgManifest m;
    int rc = pkg_manifest_parse_str(toml, &m, NULL, 0);

    ASSERT(rc == 0, "parse no-deps manifest");
    ASSERT(m.dep_count == 0, "zero deps");
}

static void
test_manifest_missing_name(void)
{
    const char *toml =
        "[package]\n"
        "version = \"1.0.0\"\n";

    PkgManifest m;
    char err[256] = {0};
    int rc = pkg_manifest_parse_str(toml, &m, err, (int)sizeof(err));
    ASSERT(rc == -1, "reject missing name");
}

static void
test_manifest_bare_version_constraint(void)
{
    const char *toml =
        "[package]\n"
        "name = \"app\"\n"
        "version = \"1.0.0\"\n"
        "\n"
        "[dependencies]\n"
        "lib = \"2.0.0\"\n";

    PkgManifest m;
    int rc = pkg_manifest_parse_str(toml, &m, NULL, 0);

    ASSERT(rc == 0, "bare version as constraint");
    ASSERT(m.deps[0].minimum.major == 2, "bare constraint major");
}

/* ── MVS resolver tests ────────────────────────────────────────────────────── */

static void
test_resolve_single(void)
{
    PkgDep deps[1];
    snprintf(deps[0].name, sizeof(deps[0].name), "web");
    deps[0].minimum = (PkgVersion){0, 3, 0};

    PkgLock lock;
    int rc = pkg_resolve(deps, 1, &lock);

    ASSERT(rc == 0, "resolve single dep");
    ASSERT(lock.count == 1, "one resolved");
    ASSERT(strcmp(lock.entries[0].name, "web") == 0, "resolved name");
    ASSERT(lock.entries[0].version.minor == 3, "resolved version");
}

static void
test_resolve_mvs_max_of_mins(void)
{
    /*
     * Two constraints on the same package:
     *   A requires web >= 0.3.0
     *   B requires web >= 0.5.0
     *
     * MVS should pick 0.5.0 (the max of all minimums).
     */
    PkgDep deps[2];
    snprintf(deps[0].name, sizeof(deps[0].name), "web");
    deps[0].minimum = (PkgVersion){0, 3, 0};
    snprintf(deps[1].name, sizeof(deps[1].name), "web");
    deps[1].minimum = (PkgVersion){0, 5, 0};

    PkgLock lock;
    int rc = pkg_resolve(deps, 2, &lock);

    ASSERT(rc == 0, "resolve conflicting constraints");
    ASSERT(lock.count == 1, "deduplicated to one");
    ASSERT(lock.entries[0].version.minor == 5, "picked max of mins (0.5.0)");
}

static void
test_resolve_multiple_packages(void)
{
    PkgDep deps[3];
    snprintf(deps[0].name, sizeof(deps[0].name), "web");
    deps[0].minimum = (PkgVersion){0, 3, 0};
    snprintf(deps[1].name, sizeof(deps[1].name), "json");
    deps[1].minimum = (PkgVersion){1, 2, 0};
    snprintf(deps[2].name, sizeof(deps[2].name), "web");
    deps[2].minimum = (PkgVersion){0, 2, 0};

    PkgLock lock;
    int rc = pkg_resolve(deps, 3, &lock);

    ASSERT(rc == 0, "resolve multi-pkg");
    ASSERT(lock.count == 2, "two distinct packages");

    /* web should be 0.3.0 (max of 0.3.0, 0.2.0) */
    int web_idx = -1, json_idx = -1;
    for (int i = 0; i < lock.count; i++) {
        if (strcmp(lock.entries[i].name, "web") == 0) web_idx = i;
        if (strcmp(lock.entries[i].name, "json") == 0) json_idx = i;
    }
    ASSERT(web_idx >= 0, "web resolved");
    ASSERT(json_idx >= 0, "json resolved");
    ASSERT(lock.entries[web_idx].version.minor == 3, "web = 0.3.0");
    ASSERT(lock.entries[json_idx].version.major == 1, "json = 1.2.0");
}

/* ── Lock file round-trip tests ────────────────────────────────────────────── */

static void
test_lock_roundtrip(void)
{
    const char *lockpath = "/tmp/test_pkg_lock.toml";

    PkgLock orig;
    memset(&orig, 0, sizeof(orig));
    orig.count = 2;
    snprintf(orig.entries[0].name, sizeof(orig.entries[0].name), "webframework");
    orig.entries[0].version = (PkgVersion){0, 3, 0};
    snprintf(orig.entries[1].name, sizeof(orig.entries[1].name), "jsonparser");
    orig.entries[1].version = (PkgVersion){1, 2, 0};

    int rc = pkg_lock_write(lockpath, &orig);
    ASSERT(rc == 0, "write lock file");

    PkgLock loaded;
    rc = pkg_lock_read(lockpath, &loaded);
    ASSERT(rc == 0, "read lock file");
    ASSERT(loaded.count == 2, "lock round-trip count");
    ASSERT(strcmp(loaded.entries[0].name, "webframework") == 0,
           "lock round-trip name[0]");
    ASSERT(loaded.entries[0].version.minor == 3, "lock round-trip ver[0]");
    ASSERT(strcmp(loaded.entries[1].name, "jsonparser") == 0,
           "lock round-trip name[1]");
    ASSERT(loaded.entries[1].version.major == 1, "lock round-trip ver[1]");

    unlink(lockpath);
}

static void
test_lock_read_missing(void)
{
    PkgLock lock;
    int rc = pkg_lock_read("/tmp/nonexistent_pkg_lock_12345.toml", &lock);
    ASSERT(rc == -1, "read missing lock file returns -1");
}

/* ── File-based manifest test ──────────────────────────────────────────────── */

static void
test_manifest_file(void)
{
    const char *path = "/tmp/test_pkg_manifest.toml";
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot create temp file\n"); return; }
    fprintf(f,
        "[package]\n"
        "name = \"filetest\"\n"
        "version = \"2.1.0\"\n"
        "\n"
        "[dependencies]\n"
        "utils = \">=1.0.0\"\n");
    fclose(f);

    PkgManifest m;
    char err[256] = {0};
    int rc = pkg_manifest_parse(path, &m, err, (int)sizeof(err));

    ASSERT(rc == 0, "parse from file");
    ASSERT(strcmp(m.name, "filetest") == 0, "file-based name");
    ASSERT(m.version.major == 2, "file-based version major");
    ASSERT(m.dep_count == 1, "file-based dep count");

    unlink(path);
}

/* ── End-to-end: parse manifest then resolve ───────────────────────────────── */

static void
test_end_to_end(void)
{
    const char *toml =
        "[package]\n"
        "name = \"myapp\"\n"
        "version = \"1.0.0\"\n"
        "\n"
        "[dependencies]\n"
        "webframework = \">=0.3.0\"\n"
        "jsonparser = \">=1.2.0\"\n";

    PkgManifest m;
    int rc = pkg_manifest_parse_str(toml, &m, NULL, 0);
    ASSERT(rc == 0, "e2e parse");

    PkgLock lock;
    rc = pkg_resolve(m.deps, m.dep_count, &lock);
    ASSERT(rc == 0, "e2e resolve");
    ASSERT(lock.count == 2, "e2e resolved count");

    /* Write and re-read */
    const char *lockpath = "/tmp/test_pkg_e2e.lock";
    rc = pkg_lock_write(lockpath, &lock);
    ASSERT(rc == 0, "e2e write lock");

    PkgLock reloaded;
    rc = pkg_lock_read(lockpath, &reloaded);
    ASSERT(rc == 0, "e2e read lock");
    ASSERT(reloaded.count == lock.count, "e2e round-trip count");

    unlink(lockpath);
}

/* ── main ──────────────────────────────────────────────────────────────────── */

int
main(void)
{
    printf("test_pkg: story 76.1.3a — MVS resolver & pkg.toml parser\n\n");

    /* SemVer */
    test_version_parse();
    test_version_cmp();
    test_version_fmt();

    /* Manifest parser */
    test_manifest_basic();
    test_manifest_comments_and_blanks();
    test_manifest_no_deps();
    test_manifest_missing_name();
    test_manifest_bare_version_constraint();
    test_manifest_file();

    /* MVS resolver */
    test_resolve_single();
    test_resolve_mvs_max_of_mins();
    test_resolve_multiple_packages();

    /* Lock file */
    test_lock_roundtrip();
    test_lock_read_missing();

    /* End-to-end */
    test_end_to_end();

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
