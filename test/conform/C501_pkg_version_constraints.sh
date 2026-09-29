#!/usr/bin/env bash
# C501_pkg_version_constraints.sh — pkg.toml version constraints mean what
# docs/reference/package-manifest.md says they mean (story 121.46, pkg item).
#
# THE DEFECTS, measured on main (e1cef7da) with this harness:
#
#   parse_constraint() read the version after ">=" with pkg_version_parse(),
#   which stopped at the first space and accepted what followed unread.  So
#   ">=1.2.0 <2.0.0" was ">=1.2.0" -- the upper bound was ignored, and a
#   graph that also asked for ">=3.1.0" resolved to 3.1.0.  The same leniency
#   accepted ">=1.0.0 garbage", "1.2.0 <2.0.0" and the empty range
#   ">=2.0.0 <1.0.0".  A bare "1.2.0", documented as "exactly 1.2.0", was a
#   minimum: next to ">=1.3.0" it resolved to 1.3.0.
#
#   The manifest's [package] version, documented as SemVer 2.0 with
#   pre-release suffixes permitted, refused "1.0.0-beta.1" and "1.0.0+b.5",
#   and accepted "01.2.3" (SemVer forbids leading zeros); a component past
#   INT_MAX was signed overflow (UBSan: runtime error).
#
# NOW: a constraint is exactly one of "X.Y.Z", ">=X.Y.Z" or
# ">=X.Y.Z <A.B.C" with A.B.C above X.Y.Z; anything else is refused (the
# manifest's E9012).  pkg_resolve() takes the highest minimum (MVS) and then
# checks it against every constraint -- an exact version it is not, or an
# upper bound it reaches, fails resolution (the manifest's E9010).  A
# [package] version is SemVer 2.0: pre-release and build suffixes are
# validated and then ignored, as documented; constraints name release
# versions only.
#
# WHAT THIS DRIVES.  A C driver over src/pkg.c's public API, under
# ASan+UBSan.  Negative control: C501_SRC=<dir> points it at another src/
# tree; main's src/ fails every case marked (defect) below and passes the
# controls.
#
# Story: 121.46

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C501_SRC:-${REPO_ROOT}/src}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c501_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C501: pkg version constraints and SemVer versions"
echo "--------------------------------------------------"

cat > "${WORK}/d.c" <<'EOF'
#include "pkg.h"
#include <stdio.h>
#include <string.h>

/* manifest with one [package] version and up to two constraints on `lib`
 * (the second from a second manifest, as a transitive dependency) */
static int parse(const char *version, const char *c, PkgManifest *m) {
    char toml[1024], err[256] = "";
    snprintf(toml, sizeof toml,
             "[package]\nname = \"app\"\nversion = \"%s\"\n\n"
             "[dependencies]\nlib = \"%s\"\n", version, c);
    return pkg_manifest_parse_str(toml, m, err, sizeof err);
}

/* 1 = both constraints parse and resolve; 0 = parse refused; -1 = resolve
 * refused.  *got = the resolved version. */
static int resolve2(const char *c1, const char *c2, char *got) {
    PkgManifest a, b;
    got[0] = 0;
    if (parse("1.0.0", c1, &a) != 0) return 0;
    if (c2 && parse("1.0.0", c2, &b) != 0) return 0;
    PkgDep deps[2]; int n = 0;
    deps[n++] = a.deps[0];
    if (c2) deps[n++] = b.deps[0];
    PkgLock lock;
    if (pkg_resolve(deps, n, &lock) != 0) return -1;
    pkg_version_fmt(lock.entries[0].version, got, 32);
    return 1;
}

static int bad = 0;
static void res(const char *name, const char *c1, const char *c2, int want,
                const char *want_ver) {
    char got[32];
    int r = resolve2(c1, c2, got);
    int ok = r == want && (!want_ver || strcmp(got, want_ver) == 0);
    const char *what[] = { "resolve refused", "parse refused", "resolved" };
    printf("%s %s: %s%s%s (want %s%s%s)\n", ok ? "OK" : "BAD", name,
           what[r + 1], r == 1 ? " " : "", r == 1 ? got : "",
           what[want + 1], want_ver ? " " : "", want_ver ? want_ver : "");
    if (!ok) bad++;
}
static void ver(const char *name, const char *v, int want_ok) {
    PkgManifest m;
    int ok = (parse(v, ">=1.0.0", &m) == 0);
    printf("%s %s: version \"%s\" %s (want %s)\n", ok == want_ok ? "OK" : "BAD",
           name, v, ok ? "accepted" : "refused", want_ok ? "accepted" : "refused");
    if (ok != want_ok) bad++;
}

int main(void) {
    /* upper bounds */
    res("range-control",          ">=1.2.0 <2.0.0", ">=1.5.0", 1, "1.5.0");
    res("range-upper-bound (defect)", ">=2.0.0 <3.0.0", ">=3.1.0", -1, NULL);
    res("range-upper-exclusive (defect)", ">=1.0.0 <2.0.0", ">=2.0.0", -1, NULL);
    res("range-empty (defect)",   ">=2.0.0 <1.0.0", NULL, 0, NULL);
    res("range-equal (defect)",   ">=2.0.0 <2.0.0", NULL, 0, NULL);
    res("trailing-garbage (defect)", ">=1.0.0 garbage", NULL, 0, NULL);
    res("bare-with-upper (defect)", "1.2.0 <2.0.0", NULL, 0, NULL);
    res("range-spaced",           ">= 1.2.0  < 2.0.0", ">=1.4.0", 1, "1.4.0");
    /* exact versions */
    res("exact-control",          "1.2.0", NULL, 1, "1.2.0");
    res("exact-and-same",         "1.2.0", ">=1.1.0", 1, "1.2.0");
    res("exact-conflict (defect)", "1.2.0", ">=1.3.0", -1, NULL);
    /* reserved and pre-release constraint syntax stays refused */
    res("caret-refused",          "^1.2.0", NULL, 0, NULL);
    res("prerelease-constraint-refused", ">=1.0.0-rc.1", NULL, 0, NULL);
    /* SemVer 2.0 [package] versions */
    ver("version-control",        "1.0.0", 1);
    ver("prerelease (defect)",    "1.0.0-beta.1", 1);
    ver("prerelease-hyphen (defect)", "1.0.0-x-y.7.z--9", 1);
    ver("build (defect)",         "1.0.0+b.5", 1);
    ver("prerelease-build (defect)", "2.1.0-rc.1+sha.5114f85", 1);
    ver("leading-zero (defect)",  "01.2.3", 0);
    ver("prerelease-leading-zero", "1.0.0-01", 0);
    ver("empty-prerelease",       "1.0.0-", 0);
    ver("empty-identifier",       "1.0.0-a..b", 0);
    ver("bad-char",               "1.0.0-a_b", 0);
    ver("overflow (defect)",      "99999999999.0.0", 0);
    return bad ? 1 : 0;
}
EOF

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c99 -g -O1 ${SAN} -iquote "${S}" -I"${S}" -o "${WORK}/d" "${WORK}/d.c" \
        "${S}/pkg.c" "${S}/sha256.c" >"${WORK}/cc.log" 2>&1; then
    fail "build: $(grep -a -m3 error "${WORK}/cc.log" | head -c 400)"
else
    ASAN_OPTIONS="detect_leaks=0" "${WORK}/d" >"${WORK}/out" 2>"${WORK}/err"; rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
    # a sanitizer abort ends the run early: name it, whatever came before
    if grep -a -q -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err"; then
        fail "sanitizer, after $(grep -a -c -E '^(OK|BAD) ' "${WORK}/out") cases -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err" | head -c 300)"
    elif [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/out"; then
        fail "exit ${rc}"
    fi
fi

echo ""
echo "C501: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
