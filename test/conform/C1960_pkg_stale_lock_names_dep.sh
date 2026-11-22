#!/usr/bin/env bash
# C1960_pkg_stale_lock_names_dep.sh — the stale-lock warning (W9010) is
# formatted from live memory and names the dependency that went stale
# (story 127.449).
#
# THE DEFECT.  pkg_build_prepare() found the stale dependency with
# lock_stale(), which returns a pointer INTO the parsed manifest
# (m->deps[k].name), then freed the manifest and only afterwards formatted
# W9010 with that pointer: a heap use-after-free on every build whose
# pkg.lock is out of date.  The ASAN+UBSAN CI job caught it in C1162 (cases
# 12a/12b); an ordinary build printed the right name only because the freed
# block had not been reused yet.
#
# NOW the warning is emitted before the manifest is freed.
#
# HOW THIS CATCHES IT WITHOUT A SANITIZER.  The run fills freed memory --
# MallocScribble on macOS (0x55), MALLOC_PERTURB_ on glibc -- so a name read
# after free() comes out as filler (or faults) instead of "greet".  The case
# asserts W9010 is a warning (rc 0), carries no fix, and names 'greet'.
#
# NO NETWORK: the package comes from a bare repository created here, over
# file:// under --allow-file-source; HOME (and so the cache) is scratch.
#
# Negative control: TKC=<tkc built from 58b1cc17> -- 1c-names-dep fails
# (the message carries the scribble bytes, not 'greet').
#
# Story: 127.449

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }
expect() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$2]"; fi; }
command -v git >/dev/null 2>&1 || { echo "SKIP C1960: git not installed"; exit 0; }
[ -x "${TKC}" ] || { echo "FAIL: tkc not found at ${TKC}"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1960_XXXXXX")"
trap 'chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT
export HOME="${WORK}/home"; mkdir -p "${HOME}"
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
export GIT_CONFIG_NOSYSTEM=1
G() { git -c user.name=t -c user.email=t@example.invalid -c init.defaultBranch=main \
          -c commit.gpgsign=false -c tag.gpgsign=false "$@"; }
R="${WORK}/repos"; mkdir -p "${R}/greet.src"
AF=--allow-file-source

printf '[package]\nname = "greet"\nversion = "1.0.0"\n' > "${R}/greet.src/pkg.toml"
printf 'm=pkg.greet;\nf=hello(n:i64):i64{ <n+1 };\n' > "${R}/greet.src/greet.tk"
G init -q --bare "${R}/greet.git"
G -C "${R}/greet.src" init -q
G -C "${R}/greet.src" add -A && G -C "${R}/greet.src" commit -qm "greet 1.0.0" \
    && G -C "${R}/greet.src" tag v1.0.0
G -C "${R}/greet.src" push -q --tags "${R}/greet.git" HEAD:refs/heads/main 2>/dev/null

P="${WORK}/app"; mkdir -p "${P}"; cd "${P}" || exit 1
printf 'm=main;\ni=g:pkg.greet;\nf=main():i64{ <g.hello(1) };\n' > main.tk
"${TKC}" pkg init app >/dev/null 2>&1
"${TKC}" pkg add greet 1.0.0 --git "file://${R}/greet.git" ${AF} >/dev/null 2>&1
"${TKC}" pkg fetch ${AF} >out 2>err; expect "0-fetch-rc" "$?" "0"

# The manifest now asks for a version the lock does not hold: a stale lock.
printf '[package]\nname = "app"\nversion = "0.1.0"\n[dependencies.greet]\nversion = "1.5.0"\ngit = "file://%s/greet.git"\n' "${R}" > pkg.toml

MallocScribble=1 MALLOC_PERTURB_=85 "${TKC}" --check ${AF} main.tk >out 2>err
expect "1a-stale-rc" "$?" "0"
W="$(grep '"error_code":"W9010"' err)"
if [ -n "${W}" ] && ! printf '%s' "${W}" | grep -q '"fix":"[^"]'; then ok "1b-W9010-no-fix"
else bad "1b-W9010-no-fix" "$(head -c 400 err)"; fi
if printf '%s' "${W}" | grep -q "out of date relative to pkg.toml ('greet')"; then ok "1c-names-dep"
else bad "1c-names-dep" "$(head -c 400 err)"; fi

echo "C1960: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
