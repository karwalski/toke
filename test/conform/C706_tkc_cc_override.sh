#!/usr/bin/env bash
# C706 -- tkc runs the clang it was built with (an absolute path), or TKC_CC;
# never a `clang` found on the caller's $PATH (story 121.28, audit-120
# compiler-toolchain COM-04; owner decision docs/owner-decisions-2026-10-01.md
# §2.10).
#
# Before: compile_binary and --emit-asm ran execvp("clang"), so a directory
# early on $PATH could substitute the compiler, and with $PATH empty no
# program could be built (E9003 "exit code 127").  Now `make` bakes the clang
# it finds (TKC_CLANG_PATH in build_info.h) and TKC_CC overrides it; an
# override that is not an executable file is refused with E9026 (rc 3)
# before clang would run.
#
# Negative control: TKC=<c7f376a7's toke> -- cases 2-6 fail
# (the planted clang runs; TKC_CC is ignored; the empty-PATH build is E9003);
# cases 1 and 7 pass there too.
#
# Story: 121.28

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
REAL_CLANG="$(command -v clang || true)"
if [ -z "${REAL_CLANG}" ]; then echo "SKIP C706: no clang on PATH"; exit 0; fi
unset TKC_CC
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c706_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
printf 'm=q;f=main():i64{<7};\n' > "${WORK}/p.tk"

# A planted `clang` early on $PATH, and a wrapper usable as TKC_CC; each
# leaves a marker and then runs the real clang.
mkdir -p "${WORK}/evil" "${WORK}/cc"
printf '#!/bin/sh\ntouch "%s/planted.ran"\nexec "%s" "$@"\n' "${WORK}" "${REAL_CLANG}" > "${WORK}/evil/clang"
printf '#!/bin/sh\ntouch "%s/override.ran"\nexec "%s" "$@"\n' "${WORK}" "${REAL_CLANG}" > "${WORK}/cc/myclang"
chmod +x "${WORK}/evil/clang" "${WORK}/cc/myclang"

codes() { grep -o '"error_code":"[A-Z0-9]*"' "$1" | sed 's/.*:"\(.*\)"/\1/' | sort -u | tr '\n' ' '; }
ran() { [ -e "${WORK}/$1" ] && echo yes || echo no; rm -f "${WORK}/$1"; }
# "rc=N codes exit=M" for a build of p.tk with the given env prefix
build() {
    rm -f "${WORK}/p"
    env "$@" "${TKC}" --out "${WORK}/p" "${WORK}/p.tk" > "${WORK}/o" 2>&1; local rc=$?
    local ex=none
    if [ "${rc}" = 0 ] && [ -x "${WORK}/p" ]; then "${WORK}/p"; ex=$?; fi
    echo "rc=${rc} $(codes "${WORK}/o")exit=${ex}"
}

r="$(build PATH="${WORK}/evil:${PATH}")"
expect "1 a planted clang first on PATH builds with the baked clang" "${r}" "rc=0 exit=7"
expect "2 ... and the planted clang never ran" "$(ran planted.ran)" "no"

r="$(build TKC_CC="${WORK}/cc/myclang")"
expect "3 TKC_CC is honoured (absolute)" "${r} $(ran override.ran)" "rc=0 exit=7 yes"
r="$(cd "${WORK}" && build TKC_CC=cc/myclang)"
expect "4 TKC_CC is honoured (relative to the current directory)" "${r} $(ran override.ran)" "rc=0 exit=7 yes"

r="$(build TKC_CC="${WORK}/no/such/clang")"
expect "5 a missing TKC_CC is E9026, rc 3, nothing built" "${r}" "rc=3 E9026 exit=none"
expect "5b ... naming the variable" "$(grep -c "TKC_CC='${WORK}/no/such/clang'" "${WORK}/o")" "1"
r="$(build TKC_CC="${WORK}/p.tk")"
expect "5c a TKC_CC that is not executable is E9026" "${r}" "rc=3 E9026 exit=none"
r="$(build TKC_CC="${WORK}/cc")"
expect "5d a TKC_CC that is a directory is E9026" "${r}" "rc=3 E9026 exit=none"
env TKC_CC="${WORK}/no/such/clang" "${TKC}" --emit-asm "${WORK}/p.tk" -o "${WORK}/p.s" > "${WORK}/o" 2>&1
expect "5e --emit-asm refuses it the same way" "rc=$? $(codes "${WORK}/o")" "rc=3 E9026 "

r="$(build PATH=)"
expect "6 the default works with PATH emptied" "${r}" "rc=0 exit=7"

env TKC_CC="${WORK}/no/such/clang" "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1
expect "7 --check runs no clang, so TKC_CC is not consulted" "rc=$?" "rc=0"

echo "C706: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
