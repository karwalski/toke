/*
 * test_pkg.c — unit tests for pkg.toml parser, MVS resolver, and lock I/O.
 *
 * Story 76.1.3a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <unistd.h>

#include "../src/pkg.h"
#include "../src/sha256.h"   /* 122.13 item 9 */

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

/* ── Supply-chain checks (story 122.10) ────────────────────────────────────── */

#define C40 "3f786850e387550fdab836ed7e6dc881de23001b"
#define C64 "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
#define H1  "sha256:2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
#define H2  "sha256:486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7"

static char tmp_path[256];

static const char *
write_tmp(const char *text)
{
    snprintf(tmp_path, sizeof(tmp_path), "%s/test_pkg_122_10_%d.lock",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    FILE *f = fopen(tmp_path, "w");
    if (!f) return tmp_path;
    fputs(text, f);
    fclose(f);
    return tmp_path;
}

static void
test_name_policy(void)
{
    ASSERT(pkg_name_valid("webframework"), "name webframework ok");
    ASSERT(pkg_name_valid("json2"), "name json2 ok");
    ASSERT(!pkg_name_valid("json_parser"), "underscore refused");
    ASSERT(!pkg_name_valid("json-parser"), "hyphen refused");
    ASSERT(!pkg_name_valid("Web"), "upper case refused");
    ASSERT(!pkg_name_valid("2web"), "leading digit refused");
    ASSERT(!pkg_name_valid(""), "empty name refused");
    ASSERT(!pkg_name_valid(NULL), "NULL name refused");
    ASSERT(!pkg_name_valid("std"), "std reserved");
    ASSERT(!pkg_name_valid("pkg"), "pkg reserved");
    ASSERT(!pkg_name_valid("toke"), "toke reserved");
    ASSERT(!pkg_name_valid("tkc"), "tkc reserved");
    ASSERT(!pkg_name_valid("a\"b"), "quote refused");
    char n63[64], n64[65];
    memset(n63, 'a', 63); n63[63] = '\0';
    memset(n64, 'a', 64); n64[64] = '\0';
    ASSERT(pkg_name_valid(n63), "63-char name ok");
    ASSERT(!pkg_name_valid(n64), "64-char name refused");

    PkgManifest m;
    char err[256] = {0};
    ASSERT(pkg_manifest_parse_str("[package]\nname = \"app\"\nversion = \"1.0.0\"\n"
                                  "[dependencies]\njson_parser = \">=1.0.0\"\n",
                                  &m, err, (int)sizeof(err)) == -1,
           "manifest: underscore dependency refused");
    ASSERT(strstr(err, "json_parser") != NULL, "manifest error names the dependency");
    ASSERT(pkg_manifest_parse_str("[package]\nname = \"std\"\nversion = \"1.0.0\"\n",
                                  &m, NULL, 0) == -1,
           "manifest: reserved package name refused");
}

static void
test_commit_hash_grammar(void)
{
    ASSERT(pkg_commit_valid(C40), "40-hex commit ok");
    ASSERT(pkg_commit_valid(C64), "64-hex commit ok");
    ASSERT(!pkg_commit_valid("main"), "branch name refused");
    ASSERT(!pkg_commit_valid("v1.2.0"), "tag name refused");
    ASSERT(!pkg_commit_valid("3f78685"), "abbreviated commit refused");
    ASSERT(!pkg_commit_valid("3F786850E387550FDAB836ED7E6DC881DE23001B"), "upper-case hex refused");
    ASSERT(!pkg_commit_valid("0000000000000000000000000000000000000000"), "null object id refused");
    ASSERT(!pkg_commit_valid(C40 "0"), "41 digits refused");
    ASSERT(!pkg_commit_valid(NULL), "NULL commit refused");
    ASSERT(pkg_hash_valid(H1), "sha256 hash ok");
    ASSERT(!pkg_hash_valid(&H1[7]), "hash without algorithm refused");
    ASSERT(!pkg_hash_valid("sha1:" C40), "sha1 refused");
    ASSERT(!pkg_hash_valid("sha256:abc"), "short hash refused");
    ASSERT(!pkg_hash_valid(NULL), "NULL hash refused");
}

static void
test_lock_pins(void)
{
    PkgLock orig;
    memset(&orig, 0, sizeof(orig));
    orig.count = 2;
    snprintf(orig.entries[0].name, sizeof(orig.entries[0].name), "webframework");
    orig.entries[0].version = (PkgVersion){0, 3, 0};
    snprintf(orig.entries[0].commit, sizeof(orig.entries[0].commit), "%s", C40);
    snprintf(orig.entries[0].hash, sizeof(orig.entries[0].hash), "%s", H1);
    snprintf(orig.entries[1].name, sizeof(orig.entries[1].name), "jsonparser");
    orig.entries[1].version = (PkgVersion){1, 2, 0};
    snprintf(orig.entries[1].commit, sizeof(orig.entries[1].commit), "%s", C64);
    snprintf(orig.entries[1].hash, sizeof(orig.entries[1].hash), "%s", H2);
    /* 121.39: a pinned entry also records its https/ssh source */
    snprintf(orig.entries[0].source, sizeof(orig.entries[0].source),
             "https://github.com/example/toke-webframework.git");
    snprintf(orig.entries[1].source, sizeof(orig.entries[1].source),
             "git@github.com:example/jsonparser.git");

    const char *path = write_tmp("");
    ASSERT(pkg_lock_write(path, &orig) == 0, "write pinned lock");
    PkgLock back;
    ASSERT(pkg_lock_read(path, &back) == 0, "read pinned lock");
    ASSERT(back.count == 2, "pinned lock count");
    ASSERT(strcmp(back.entries[0].commit, C40) == 0, "commit[0] round-trips");
    ASSERT(strcmp(back.entries[0].hash, H1) == 0, "hash[0] round-trips");
    ASSERT(strcmp(back.entries[1].commit, C64) == 0, "commit[1] round-trips");
    ASSERT(strcmp(back.entries[1].hash, H2) == 0, "hash[1] round-trips");
    ASSERT(strcmp(back.entries[0].source, orig.entries[0].source) == 0, "source[0] round-trips");
    ASSERT(strcmp(back.entries[1].source, orig.entries[1].source) == 0, "source[1] round-trips");

    char err[256] = {0};
    ASSERT(pkg_lock_check_pinned(&back, err, (int)sizeof(err)) == 0, "fully pinned lock accepted");
    ASSERT(pkg_lock_verify(&back.entries[0], C40, H1, err, (int)sizeof(err)) == 0,
           "verify: matching commit+hash accepted");
    err[0] = '\0';
    ASSERT(pkg_lock_verify(&back.entries[0], C64, H1, err, (int)sizeof(err)) == -1,
           "verify: moved tag (other commit) refused");
    ASSERT(strstr(err, "commit mismatch") != NULL, "verify names the commit mismatch");
    err[0] = '\0';
    ASSERT(pkg_lock_verify(&back.entries[0], C40, H2, err, (int)sizeof(err)) == -1,
           "verify: tampered content (other hash) refused");
    ASSERT(strstr(err, "content hash mismatch") != NULL, "verify names the hash mismatch");

    /* 121.39: no source, no fetch */
    {
        PkgLock nosrc = back;
        nosrc.entries[1].source[0] = '\0';
        err[0] = '\0';
        ASSERT(pkg_lock_check_pinned(&nosrc, err, (int)sizeof(err)) == -1,
               "entry with no source refused");
        ASSERT(strstr(err, "jsonparser") && strstr(err, "source"),
               "no-source error names the package and the source");
        ASSERT(pkg_lock_verify(&nosrc.entries[1], C64, H2, err, (int)sizeof(err)) == -1,
               "verify against an entry with no source refused");
        nosrc.entries[1] = back.entries[1];
        snprintf(nosrc.entries[1].source, sizeof(nosrc.entries[1].source),
                 "http://github.com/example/jsonparser.git");
        ASSERT(pkg_lock_write(path, &nosrc) == -1, "writer refuses an http source");
    }

    /* unpinned entries */
    back.entries[1].hash[0] = '\0';
    err[0] = '\0';
    ASSERT(pkg_lock_check_pinned(&back, err, (int)sizeof(err)) == -1, "entry with no hash refused");
    ASSERT(strstr(err, "jsonparser") != NULL, "unpinned error names the package");
    back.entries[0].commit[0] = '\0';
    ASSERT(pkg_lock_check_pinned(&back, err, (int)sizeof(err)) == -1, "entry with no commit refused");
    ASSERT(pkg_lock_verify(&back.entries[0], C40, H1, err, (int)sizeof(err)) == -1,
           "verify against an unpinned entry refused");

    /* a resolve-only lock (no pins) is still writable and readable */
    PkgLock plain;
    memset(&plain, 0, sizeof(plain));
    plain.count = 1;
    snprintf(plain.entries[0].name, sizeof(plain.entries[0].name), "web");
    plain.entries[0].version = (PkgVersion){0, 1, 0};
    ASSERT(pkg_lock_write(path, &plain) == 0, "resolve-only lock written");
    ASSERT(pkg_lock_read(path, &back) == 0 && back.count == 1 && back.entries[0].commit[0] == '\0',
           "resolve-only lock reads back unpinned");
    ASSERT(pkg_lock_check_pinned(&back, NULL, 0) == -1, "resolve-only lock refused for fetch");

    /* the writer refuses to inject */
    PkgLock evil = plain;
    snprintf(evil.entries[0].name, sizeof(evil.entries[0].name), "web\"\n[commit]\nweb");
    unlink(path);
    ASSERT(pkg_lock_write(path, &evil) == -1, "writer refuses a name that would inject lines");
    FILE *f = fopen(path, "r");
    ASSERT(f == NULL, "refused write creates no file");
    if (f) fclose(f);
    evil = plain;
    snprintf(evil.entries[0].commit, sizeof(evil.entries[0].commit), "main");
    ASSERT(pkg_lock_write(path, &evil) == -1, "writer refuses a branch-name commit");
    unlink(path);
}

static void
test_lock_read_rejects(void)
{
    PkgLock l;
    static const struct { const char *text; const char *what; } bad[] = {
        { "[resolved]\nweb = \"0.1.0\"\n[commit]\nother = \"" C40 "\"\n", "pin for an unresolved package" },
        { "[resolved]\nweb = \"0.1.0\"\n[commit]\nweb = \"main\"\n", "branch name as commit" },
        { "[resolved]\nweb = \"0.1.0\"\n[commit]\nweb = \"3f78685\"\n", "abbreviated commit" },
        { "[resolved]\nweb = \"0.1.0\"\n[hash]\nweb = \"md5:abc\"\n", "non-sha256 hash" },
        { "[resolved]\nweb = \"0.1.0\"\nweb = \"0.2.0\"\n", "duplicate resolved entry" },
        { "[resolved]\nweb = \"0.1.0\"\n[commit]\nweb = \"" C40 "\"\nweb = \"" C64 "\"\n", "commit pinned twice" },
        { "[resolved]\nweb_x = \"0.1.0\"\n", "invalid name in [resolved]" },
        { "lock-version = \"3\"\n[resolved]\nweb = \"0.1.0\"\n", "unknown lock-version" },
        { "[resolved]\nweb = \"0.1\"\n", "malformed version" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const char *path = write_tmp(bad[i].text);
        char msg[160];
        snprintf(msg, sizeof(msg), "reader refuses: %s", bad[i].what);
        ASSERT(pkg_lock_read(path, &l) == -1, msg);
    }
    /* pre-122.10 lock (no lock-version, no pins) still reads */
    const char *path = write_tmp("# old\n[resolved]\nweb = \"0.3.0\"\n");
    ASSERT(pkg_lock_read(path, &l) == 0 && l.count == 1 && l.entries[0].commit[0] == '\0',
           "pre-122.10 lock reads, unpinned");
    unlink(path);
}

/* ── main ──────────────────────────────────────────────────────────────────── */

/* ── Shared SHA-256 (122.13 item 9) ────────────────────────────────────────── */

/* FIPS 180-2 appendix B known answers, plus the empty string.  The digest is
 * the one the package content hash is built from, so a content hash formed
 * from it must satisfy pkg_hash_valid. */
static void
test_sha256_known_answers(void)
{
    char hex[65];
    tk_sha256_hex("", 0, hex);
    ASSERT(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
           "sha256(\"\")");
    tk_sha256_hex("abc", 3, hex);
    ASSERT(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
           "sha256(\"abc\")");
    const char *m2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    tk_sha256_hex(m2, strlen(m2), hex);
    ASSERT(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
           "sha256(448-bit message)");
    /* one million 'a', fed in uneven pieces through the streaming API */
    TkSha256 ctx; unsigned char d[32]; char chunk[997];
    memset(chunk, 'a', sizeof chunk);
    tk_sha256_init(&ctx);
    unsigned long left = 1000000;
    while (left) {
        unsigned long n = left < sizeof chunk ? left : (unsigned long)sizeof chunk;
        tk_sha256_update(&ctx, (const unsigned char *)chunk, n);
        left -= n;
    }
    tk_sha256_final(&ctx, d);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { hex[2*i] = hx[d[i] >> 4]; hex[2*i+1] = hx[d[i] & 15]; }
    hex[64] = '\0';
    ASSERT(!strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
           "sha256(10^6 x 'a') streamed");
    char pin[PKG_MAX_HASH];
    tk_sha256_hex("abc", 3, hex);
    snprintf(pin, sizeof pin, "sha256:%s", hex);
    ASSERT(pkg_hash_valid(pin), "a content hash formed from tk_sha256 is a valid pin");
}

/* 76.1.3d: file:// only under the explicit override, and only absolute. */
static void
test_file_source_override(void)
{
    ASSERT(!pkg_allow_file_sources(), "override is off by default");
    ASSERT(!pkg_source_valid("file:///srv/repo.git"), "file:// refused by default");
    pkg_set_allow_file_sources(true);
    ASSERT(pkg_source_valid("file:///srv/repo.git"), "file:/// accepted under the override");
    ASSERT(!pkg_source_valid("file://host/repo.git"), "file://host refused (not absolute)");
    ASSERT(!pkg_source_valid("file:///srv/../etc"), "a .. segment refused");
    ASSERT(!pkg_source_valid("file:///srv/.."), "a trailing .. refused");
    ASSERT(!pkg_source_valid("file:///-x"), "a path beginning with '-' refused");
    ASSERT(!pkg_source_valid("file:///"), "an empty path refused");
    ASSERT(!pkg_source_valid("file:///a b"), "whitespace still refused");
    ASSERT(!pkg_source_valid("ext::sh -c x"), "ext:: still refused");
    ASSERT(!pkg_source_valid("http://example.com/x.git"), "http still refused");
    ASSERT(!pkg_source_valid("/srv/repo.git"), "a bare local path still refused");
    pkg_set_allow_file_sources(false);
    ASSERT(!pkg_source_valid("file:///srv/repo.git"), "refused again when the override is off");
}

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

    /* Supply-chain checks (122.10) */
    test_name_policy();
    test_commit_hash_grammar();
    test_lock_pins();
    test_lock_read_rejects();
    test_file_source_override();

    /* Shared SHA-256 (122.13 item 9) */
    test_sha256_known_answers();

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
