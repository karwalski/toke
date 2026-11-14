#!/usr/bin/env bash
# C492_pkg_integrity_by_construction.sh — the package manager cannot be
# pointed at an unauthenticated source, cannot declare an install hook, and
# verifies a fetched tree by hashing it itself (story 121.39, audit N9;
# design in ADR-0004 "Supply-chain security", 122.10).
#
# THE DEFECTS, measured on main (1346588c) with this harness -- all before
# any fetch code exists, which is the point: fetch is to be built on top of
# these checks, not retrofitted with them.
#
#   hooks     pkg.toml parsed any table and any key and ignored what it did
#             not know: `[scripts] postinstall = "sh evil.sh"` and
#             `build = "make"` under [package] were accepted in silence --
#             an ignored key is one a later version can start to honour.
#             A line with no `=` was skipped.
#   sources   the documented extended form `[dependencies.<name>]` with a
#             `git` URL was dropped in silence (the dependency vanished), so
#             no source was ever checked: http://, git://, file://, ext::
#             (which runs a command) and `--upload-pack=...` were all
#             "accepted".  Duplicate dependencies were accepted.
#   lock      a lock entry needed a commit and a hash, but nothing recorded
#             WHERE they came from, so a fetch could take the URL from the
#             index again on every build (the dependency-confusion path the
#             ADR closes).  `[source]` did not exist; a lock with an http://
#             source read cleanly.
#   hash      the canonical tree hash was defined in the ADR but nothing
#             computed it, so the only verifier, pkg_lock_verify, compared
#             two strings the caller supplied.
#
# WHAT THIS DRIVES.  Two C harnesses against src/pkg.c and src/sha256.c,
# under ASan+UBSan.  Part A uses only the API that exists on main, so it
# builds there and reports each case.  Part B uses the new API
# (pkg_source_valid, pkg_tree_hash, pkg_verify_fetched); on main it does not
# build, which is reported as one FAIL.  The tree-hash expectation is
# computed independently in the harness from the ADR's definition.
#
# Negative control: C492_SRC=<dir> points both harnesses at another src/
# tree; main's fails every case in part A except the two controls, and part B
# does not build.
#
# Story: 121.39

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
# 127.439: the project's gcc policy (Makefile GCC_CHECK): snprintf truncation
# is by design, so -Wformat-truncation is off where the compiler takes the flag.
NOFT=""; "${CC}" -Wno-format-truncation -x c -c /dev/null -o /dev/null 2>/dev/null && NOFT="-Wno-format-truncation"
S="${C492_SRC:-${REPO_ROOT}/src}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c492_XXXXXX")"
trap 'chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C492: package integrity by construction"
echo "---------------------------------------"

# ── Part A: manifest and lock rules, on the API main already has ─────────
cat > "${WORK}/a.c" <<'EOF'
#include "pkg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C40 "3f786850e387550fdab836ed7e6dc881de23001b"
#define H1  "sha256:2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
#define PK  "[package]\nname = \"app\"\nversion = \"1.0.0\"\n"

static int bad;
static void man(const char *name, const char *toml, int want_rc, int want_deps) {
    PkgManifest m; char err[256] = "";
    int rc = pkg_manifest_parse_str(toml, &m, err, (int)sizeof err);
    int ok = rc == want_rc && (want_rc != 0 || want_deps < 0 || m.dep_count == want_deps);
    printf("%s manifest %s: rc %d (want %d)%s%s\n", ok ? "OK" : "BAD", name, rc, want_rc,
           rc ? " -- " : "", rc ? err : "");
    if (!ok) bad++;
}
static const char *tmpf(const char *text) {
    static char p[512];
    snprintf(p, sizeof p, "%s/c492_%d.lock", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    FILE *f = fopen(p, "w"); fputs(text, f); fclose(f);
    return p;
}
int main(void) {
    man("scripts-table-postinstall", PK "[scripts]\npostinstall = \"sh evil.sh\"\n", -1, 0);
    man("build-key-under-package", PK "build = \"make\"\n", -1, 0);
    man("line-without-equals", PK "[dependencies]\nevil\n", -1, 0);
    man("top-level-key", "hook = \"x\"\n" PK, -1, 0);
    man("duplicate-dependency", PK "[dependencies]\nweb = \">=1.0.0\"\nweb = \">=2.0.0\"\n", -1, 0);
    man("junk-after-quoted-value", PK "[dependencies]\nweb = \">=1.0.0\" ; rm -rf ~\n", -1, 0);
    man("extended-https-kept", PK "[dependencies.web]\nversion = \">=0.3.0\"\n"
        "git = \"https://github.com/example/toke-web.git\"\n", 0, 1);
    man("extended-http-refused", PK "[dependencies.web]\nversion = \">=0.3.0\"\n"
        "git = \"http://github.com/example/toke-web.git\"\n", -1, 0);
    man("extended-ext-transport-refused", PK "[dependencies.web]\nversion = \">=0.3.0\"\n"
        "git = \"ext::sh -c touch% /tmp/pwned\"\n", -1, 0);
    man("extended-option-injection-refused", PK "[dependencies.web]\nversion = \">=0.3.0\"\n"
        "git = \"--upload-pack=touch /tmp/pwned\"\n", -1, 0);
    man("extended-missing-version", PK "[dependencies.web]\n"
        "git = \"https://github.com/example/toke-web.git\"\n", -1, 0);
    man("extended-duplicates-simple", PK "[dependencies]\nweb = \">=1.0.0\"\n"
        "[dependencies.web]\nversion = \">=0.3.0\"\n", -1, 0);
    man("control-simple-form", PK "[dependencies]\nweb = \">=1.0.0\"\njson = \"1.2.0\"\n", 0, 2);

    /* lock: a pinned entry must also say where it came from */
    PkgLock l; char err[256] = "";
    const char *p = tmpf("lock-version = \"2\"\n[resolved]\nweb = \"0.3.0\"\n[commit]\nweb = \""
                         C40 "\"\n[hash]\nweb = \"" H1 "\"\n");
    int rr = pkg_lock_read(p, &l);
    int rc = rr == 0 ? pkg_lock_check_pinned(&l, err, (int)sizeof err) : -2;
    printf("%s lock pinned-without-source refused for fetch: read %d, check %d (want 0, -1)\n",
           rr == 0 && rc == -1 ? "OK" : "BAD", rr, rc);
    if (!(rr == 0 && rc == -1)) bad++;

    p = tmpf("lock-version = \"2\"\n[resolved]\nweb = \"0.3.0\"\n[source]\n"
             "web = \"http://github.com/example/toke-web.git\"\n");
    rr = pkg_lock_read(p, &l);
    printf("%s lock http-source refused by the reader: read %d (want -1)\n", rr == -1 ? "OK" : "BAD", rr);
    if (rr != -1) bad++;

    p = tmpf("lock-version = \"2\"\n[resolved]\nweb = \"0.3.0\"\n[postfetch]\nweb = \"sh x\"\n");
    rr = pkg_lock_read(p, &l);
    printf("%s lock unknown-table refused: read %d (want -1)\n", rr == -1 ? "OK" : "BAD", rr);
    if (rr != -1) bad++;

    p = tmpf("lock-version = \"2\"\n[resolved]\nweb = \"0.3.0\"\n");
    rr = pkg_lock_read(p, &l);
    printf("%s lock control-resolve-only-reads: read %d (want 0)\n", rr == 0 ? "OK" : "BAD", rr);
    if (rr != 0) bad++;
    unlink(p);
    return bad ? 1 : 0;
}
EOF

# ── Part B: the new API ───────────────────────────────────────────────────
cat > "${WORK}/b.c" <<'EOF'
#include "pkg.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int bad;
static void chk(int ok, const char *what) { printf("%s %s\n", ok ? "OK" : "BAD", what); if (!ok) bad++; }

static void put(const char *root, const char *rel, const char *text) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w"); if (f) { fputs(text, f); fclose(f); }
}
/* The ADR-0004 canonical tree hash, computed here from the definition:
 * files sorted by path bytes, each "path NUL size NUL bytes". */
static void expect(char out[PKG_MAX_HASH], const char *const *paths,
                   const char *const *texts, int n) {
    TkSha256 c; tk_sha256_init(&c);
    for (int i = 0; i < n; i++) {
        char sz[32]; int k = snprintf(sz, sizeof sz, "%zu", strlen(texts[i]));
        tk_sha256_update(&c, (const unsigned char *)paths[i], strlen(paths[i]) + 1);
        tk_sha256_update(&c, (const unsigned char *)sz, (unsigned long)k + 1);
        tk_sha256_update(&c, (const unsigned char *)texts[i], strlen(texts[i]));
    }
    unsigned char d[32]; tk_sha256_final(&c, d);
    static const char hx[] = "0123456789abcdef";
    memcpy(out, "sha256:", 7);
    for (int i = 0; i < 32; i++) { out[7+2*i] = hx[d[i] >> 4]; out[8+2*i] = hx[d[i] & 15]; }
    out[71] = '\0';
}

int main(int argc, char **argv) {
    (void)argc;
    const char *root = argv[1];

    /* sources */
    static const char *const good[] = {
        "https://github.com/example/toke-web.git", "https://git.example.org:8443/a/b",
        "ssh://git@github.com/example/toke-web.git", "ssh://github.com:2222/a/b.git",
        "git@github.com:example/toke-web.git", NULL };
    static const char *const evil[] = {
        "http://github.com/example/toke-web.git", "git://github.com/example/toke-web.git",
        "file:///tmp/repo", "/tmp/repo", "./repo", "../repo", "ext::sh -c touch% /tmp/pwned",
        "--upload-pack=touch /tmp/pwned", "-oProxyCommand=touch /tmp/pwned",
        "ssh://-oProxyCommand=touch/a", "git@-oProxyCommand=x:a/b",
        "https://user:token@github.com/a/b", "https://token@github.com/a/b",
        "https:///a/b", "https://github.com", "https://github.com/a b", "https://github.com/a\"b",
        "git+ssh://github.com/a/b", "ssh+git://github.com/a/b", "fd::17", "github.com:a/b\n",
        "", NULL };
    for (int i = 0; good[i]; i++) {
        char w[160]; snprintf(w, sizeof w, "source accepted: %s", good[i]);
        chk(pkg_source_valid(good[i]), w);
    }
    for (int i = 0; evil[i]; i++) {
        char w[160]; snprintf(w, sizeof w, "source refused: %s", evil[i]);
        chk(!pkg_source_valid(evil[i]), w);
    }
    chk(!pkg_source_valid(NULL), "source refused: NULL");

    /* tree hash: an independent computation from the ADR's definition */
    char p[1024], h[PKG_MAX_HASH], want[PKG_MAX_HASH], err[512];
    snprintf(p, sizeof p, "%s/t", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/t/sub", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/t/.git", root); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/t", root);
    put(p, "web.tk", "m=web;\n"); put(p, "sub/b.tk", "x"); put(p, "a-b.tk", "");
    put(p, ".git/HEAD", "ref: refs/heads/main\n");
    const char *paths[] = { "a-b.tk", "sub/b.tk", "web.tk" };
    const char *texts[] = { "", "x", "m=web;\n" };
    expect(want, paths, texts, 3);
    int rc = pkg_tree_hash(p, h, err, (int)sizeof err);
    chk(rc == 0 && strcmp(h, want) == 0, "tree hash equals the ADR definition (root .git skipped)");
    printf("   got  %s\n   want %s\n", rc == 0 ? h : err, want);
    chk(pkg_hash_valid(h), "tree hash is a valid pin");

    char q[1024]; snprintf(q, sizeof q, "%s/t/empty", root); mkdir(q, 0755);
    chk(pkg_tree_hash(p, h, err, (int)sizeof err) == 0 && strcmp(h, want) == 0,
        "an empty directory does not change the hash (git does not track it)");
    rmdir(q);

    /* verify: the caller hands over a DIRECTORY, not a hash string */
    PkgResolved e; memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "web");
    snprintf(e.source, sizeof e.source, "https://github.com/example/toke-web.git");
    snprintf(e.commit, sizeof e.commit, "3f786850e387550fdab836ed7e6dc881de23001b");
    snprintf(e.hash, sizeof e.hash, "%s", want);
    err[0] = '\0';
    chk(pkg_verify_fetched(&e, e.commit, p, err, (int)sizeof err) == 0, "verify: untouched tree accepted");
    put(p, "sub/b.tk", "y");
    err[0] = '\0';
    chk(pkg_verify_fetched(&e, e.commit, p, err, (int)sizeof err) == -1 &&
        strstr(err, "content hash mismatch"), "verify: one changed byte refused");
    put(p, "sub/b.tk", "x");
    snprintf(q, sizeof q, "%s/t/sub/b.tk", root);
    char q2[1024]; snprintf(q2, sizeof q2, "%s/t/sub/c.tk", root);
    rename(q, q2);
    chk(pkg_verify_fetched(&e, e.commit, p, err, (int)sizeof err) == -1, "verify: a renamed file refused");
    rename(q2, q);
    chk(pkg_verify_fetched(&e, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b", p, err, (int)sizeof err) == -1,
        "verify: another commit refused");
    PkgResolved nos = e; nos.source[0] = '\0';
    chk(pkg_verify_fetched(&nos, e.commit, p, err, (int)sizeof err) == -1, "verify: an entry with no source refused");

    /* what the tree may not contain */
    snprintf(q, sizeof q, "%s/t/link.tk", root);
    if (symlink("/etc/passwd", q) == 0) {
        err[0] = '\0';
        chk(pkg_tree_hash(p, h, err, (int)sizeof err) == -1 && strstr(err, "link.tk"),
            "tree hash: a symlink is refused, and named");
        unlink(q);
    } else chk(0, "could not create a symlink");
    snprintf(q, sizeof q, "%s/t/fifo", root);
    if (mkfifo(q, 0644) == 0) {
        chk(pkg_tree_hash(p, h, err, (int)sizeof err) == -1, "tree hash: a FIFO is refused (and not opened)");
        unlink(q);
    } else chk(0, "could not create a FIFO");
    snprintf(q, sizeof q, "%s/t/sub/.git", root); mkdir(q, 0755);
    chk(pkg_tree_hash(p, h, err, (int)sizeof err) == -1, "tree hash: a nested .git is refused");
    rmdir(q);
    snprintf(q, sizeof q, "%s/t/sub/secret.tk", root);
    put(p, "sub/secret.tk", "s"); chmod(q, 0);
    if (access(q, R_OK) != 0) {
        chk(pkg_tree_hash(p, h, err, (int)sizeof err) == -1, "tree hash: an unreadable file is an error, not skipped");
    }
    chmod(q, 0644); unlink(q);
    chk(pkg_tree_hash(p, h, err, (int)sizeof err) == 0 && strcmp(h, want) == 0,
        "control: the restored tree hashes as before");
    snprintf(q, sizeof q, "%s/missing", root);
    chk(pkg_tree_hash(q, h, err, (int)sizeof err) == -1, "tree hash: a missing directory is an error");
    return bad ? 1 : 0;
}
EOF

run_part() {
    local name="$1"; shift
    if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror ${NOFT} -g -O1 ${SAN} -iquote "${S}" \
            -o "${WORK}/${name}" "${WORK}/${name}.c" "${S}/pkg.c" "${S}/sha256.c" \
            >"${WORK}/${name}.cc" 2>&1; then
        fail "part ${name}: does not build -- $(grep -a -m2 error "${WORK}/${name}.cc" | head -c 300)"
        return
    fi
    mkdir -p "${WORK}/${name}.root"
    TMPDIR="${WORK}" "${WORK}/${name}" "${WORK}/${name}.root" >"${WORK}/${name}.out" 2>"${WORK}/${name}.err"
    local rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/${name}.out")
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/${name}.out"; then
        fail "part ${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/${name}.err" | head -c 300)"
    fi
}
run_part a
run_part b

echo ""
echo "C492: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
