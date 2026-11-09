#!/usr/bin/env bash
# C1162_pkg_imports_through_lock.sh — `i=x:pkg.<name>[.<module>]` resolves
# through pkg.lock and the verified package cache (story 76.1.3c; ADR-0004
# "Compiler Integration").
#
# THE GAP. A pkg.* import was looked up as an ordinary user module
# (./pkg/<name>.tki) and failed with E2030 "module not found"; nothing read
# pkg.toml or pkg.lock at build time, so the lock's pins protected nothing.
#
# WHAT IS CHECKED. A program importing two packages (one transitive through
# the other) and a package submodule builds and runs; --check resolves the
# same imports.  Every refusal is reported AT THE IMPORT as one structured
# diagnostic: no pkg.toml (E9011), no lock (E9028), an unpinned lock or a
# package missing from the cache (E9029), a cache that no longer hashes to
# the lock (E9030 -- the cache is re-verified on every build), a package
# module that does not declare its import path (E9035), and a package that
# is not a dependency (E9011).  A program with no pkg.* import is untouched
# by a broken pkg.toml beside it.
#
# NO NETWORK: packages come from bare repositories created here, over
# file:// under --allow-file-source; HOME (and so the cache) is scratch.
#
# Negative control: TKC=<tkc built from main without 76.1.3b-d> -- 21 of 26 fail:
# the build (E2030 at the import) and every diagnostic case; the 5 that pass
# are the plain program and rc-only checks.
#
# Story: 76.1.3c

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$2]"; fi; }
# diag_at <name> <file> <code> <line>: the code, at that line of main.tk, no fix
diag_at() {
    if grep -q '"error_code":"'"$3"'"' "$2" && grep -q '"file":"main.tk","pos":{"offset":[0-9]*,"line":'"$4"',' "$2" \
       && ! grep -q '"fix":"[^"]' "$2"; then ok "$1"
    else bad "$1" "want $3 at main.tk:$4 in: $(head -c 400 "$2")"; fi
}
command -v git >/dev/null 2>&1 || { echo "SKIP C1162: git not installed"; exit 0; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1162_XXXXXX")"
trap 'chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT
export HOME="${WORK}/home"; mkdir -p "${HOME}"
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
export GIT_CONFIG_NOSYSTEM=1
G() { git -c user.name=t -c user.email=t@example.invalid -c init.defaultBranch=main \
          -c commit.gpgsign=false -c tag.gpgsign=false "$@"; }
R="${WORK}/repos"; mkdir -p "${R}"
CACHE="${HOME}/.toke/pkg"
AF=--allow-file-source
publish() {   # publish <name> <version>: commit $R/<name>.src, tag, push to a bare repo
    local d="${R}/$1.src"
    [ -d "${R}/$1.git" ] || G init -q --bare "${R}/$1.git"
    [ -d "${d}/.git" ] || G -C "${d}" init -q
    G -C "${d}" add -A && G -C "${d}" commit -qm "$1 $2" && G -C "${d}" tag "v$2"
    G -C "${d}" push -q --tags "${R}/$1.git" HEAD:refs/heads/main 2>/dev/null
}

mkdir -p "${R}/util.src" "${R}/greet.src"
printf '[package]\nname = "util"\nversion = "1.0.0"\n' > "${R}/util.src/pkg.toml"
printf 'm=pkg.util;\nf=twice(n:i64):i64{ <n*2 };\n' > "${R}/util.src/util.tk"
publish util 1.0.0
printf '[package]\nname = "greet"\nversion = "1.0.0"\n[dependencies.util]\nversion = "1.0.0"\ngit = "file://%s/util.git"\n' "${R}" > "${R}/greet.src/pkg.toml"
printf 'm=pkg.greet;\ni=u:pkg.util;\nf=hello(n:i64):i64{ <u.twice(n)+1 };\n' > "${R}/greet.src/greet.tk"
printf 'm=pkg.greet.fmt;\nf=wrap(s:str):str{ <"[\\(s)]" };\n' > "${R}/greet.src/fmt.tk"
publish greet 1.0.0

P="${WORK}/app"; mkdir -p "${P}"; cd "${P}" || exit 1
cat > main.tk <<'EOF'
m=main;
i=g:pkg.greet;
i=gf:pkg.greet.fmt;
i=io:std.io;
f=main():i64{
  io.println(gf.wrap("hi"));
  <g.hello(20)
};
EOF

# 3 (first, before any manifest). no pkg.toml: E9011 at the import
"${TKC}" --check ${AF} main.tk >out 2>err; expect "3a-no-manifest-rc" "$?" "1"
diag_at "3b-no-manifest-E9011" err E9011 2

"${TKC}" pkg init app >/dev/null 2>&1
"${TKC}" pkg add greet 1.0.0 --git "file://${R}/greet.git" ${AF} >/dev/null 2>&1
# 5. a resolved but unfetched lock has no pins: E9029
"${TKC}" --check ${AF} main.tk >out 2>err; expect "5a-unpinned-rc" "$?" "1"
diag_at "5b-unpinned-E9029" err E9029 2
# 4. no lock at all: E9028
mv pkg.lock lock.off
"${TKC}" --check ${AF} main.tk >out 2>err; diag_at "4-no-lock-E9028" err E9028 2
mv lock.off pkg.lock

"${TKC}" pkg fetch ${AF} >out 2>err; expect "0-fetch-rc" "$?" "0"

# 1. build and run: entry module, submodule, a package's own package import
rm -f b; "${TKC}" ${AF} --out b main.tk >out 2>err; expect "1a-build-rc" "$?" "0"
[ -x b ] && { OUT="$(./b)"; RC=$?; } || { OUT=""; RC=-1; }
expect "1b-run-output" "${OUT}" "[hi]"
expect "1c-run-exit" "${RC}" "41"
# 2. --check resolves the same imports
"${TKC}" --check ${AF} main.tk >out 2>err; expect "2-check-rc" "$?" "0"
# 2b. without the override the file:// lock is refused, at the import
"${TKC}" --check main.tk >out 2>err; expect "2b-no-override-rc" "$?" "1"
grep -q '"file":"main.tk"' err && ok "2c-no-override-at-import" || bad "2c-no-override-at-import" "$(cat err)"

# 9. a package that is not a dependency: E9011 at its import
printf 'm=main;\ni=x:pkg.other;\nf=main():i64{ <0 };\n' > other.tk
"${TKC}" --check ${AF} other.tk >out 2>err; expect "9a-not-dep-rc" "$?" "1"
grep -q '"error_code":"E9011"' err && grep -q '"line":2' err && ok "9b-not-dep-E9011" || bad "9b-not-dep-E9011" "$(cat err)"

# 12. a stale lock is a warning (W9010), not a silent re-resolve
cp pkg.toml toml.good
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.greet]\nversion = "1.5.0"\ngit = "file://%s/greet.git"\n' "${R}" > pkg.toml
"${TKC}" --check ${AF} main.tk >out 2>err; expect "12a-stale-rc" "$?" "0"
grep -q '"error_code":"W9010"' err && ok "12b-stale-W9010" || bad "12b-stale-W9010" "$(cat err)"
cp toml.good pkg.toml

# 7. the cache is re-verified on every build: a changed file is refused
cp "${CACHE}/util/1.0.0/util.tk" util.keep
printf 'm=pkg.util;\nf=twice(n:i64):i64{ <n*3 };\n' > "${CACHE}/util/1.0.0/util.tk"
"${TKC}" ${AF} --out b main.tk >out 2>err; expect "7a-tamper-rc" "$?" "1"
diag_at "7b-tamper-E9030" err E9030 2
cp util.keep "${CACHE}/util/1.0.0/util.tk"
# 6. a package missing from the cache: E9029
mv "${CACHE}/util" "${WORK}/util.off"
"${TKC}" --check ${AF} main.tk >out 2>err; diag_at "6-not-cached-E9029" err E9029 2
mv "${WORK}/util.off" "${CACHE}/util"
"${TKC}" --check ${AF} main.tk >out 2>err; expect "6b-restored-rc" "$?" "0"

# 8. a package module that does not declare its import path: E9035
mkdir -p "${R}/bad.src"
printf '[package]\nname = "bad"\nversion = "1.0.0"\n' > "${R}/bad.src/pkg.toml"
printf 'm=bad;\nf=one():i64{ <1 };\n' > "${R}/bad.src/bad.tk"
publish bad 1.0.0
"${TKC}" pkg add bad 1.0.0 --git "file://${R}/bad.git" ${AF} >/dev/null 2>&1
"${TKC}" pkg fetch ${AF} >/dev/null 2>&1
printf 'm=main;\ni=b:pkg.bad;\nf=main():i64{ <b.one() };\n' > main.tk
"${TKC}" --check ${AF} main.tk >out 2>err; expect "8a-bad-module-rc" "$?" "1"
diag_at "8b-bad-module-E9035" err E9035 2
grep -q 'm=pkg.bad;' err && ok "8c-names-expected" || bad "8c-names-expected" "$(cat err)"

# 10. a program with no pkg.* import ignores a broken pkg.toml beside it
printf '[package]\nname = "app"\n[scripts]\nx = "y"\n' > pkg.toml
printf 'm=main;\nf=main():i64{ <7 };\n' > plain.tk
rm -f p; "${TKC}" --out p plain.tk >out 2>err; expect "10a-plain-build-rc" "$?" "0"
./p; expect "10b-plain-run" "$?" "7"
[ ! -s err ] && ok "10c-plain-silent" || bad "10c-plain-silent" "$(cat err)"

echo "C1162: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
