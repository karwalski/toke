#!/usr/bin/env bash
# C680 -- `--check` refuses the caps the build refuses (127.264 (1)).
#
# Codegen sizes seven registries from the caps (max_funcs, max_structs,
# max_imports, and four from max_locals) and refuses, with E9010, caps whose
# registry is larger than one arena allocation can be (127.258, C641).  That
# refusal lived only in codegen, so `tkc --check --max-funcs=1000000` said
# rc 0 while the build of the same file refused it.  The size bound is now
# checked after import resolution, where both paths pass, with the same
# diagnostic and the build's exit status.
#
# The counted caps treated a cap of 0 as "no cap" (`cap > 0`) while codegen
# allocated zero entries, so `--max-funcs=0` also passed --check and failed
# the build.  A cap of 0 is now counted like any other.
#
# Every case runs --check and the build and requires the same outcome.
#
# Negative control: TKC=<437dc471's toke> -- cases 1-6 fail (check rc=0
# while the build refuses).
#
# Story: 127.264

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
printf 'm=q;\nf=a():i64{<1};\nf=main():i64{<a()};\n' > "${WORK}/p.tk"

# "check=rc codes | build=rc codes" for one flag
both() {
    "${TKC}" --check "$1" "${WORK}/p.tk" > "${WORK}/c" 2>&1; crc=$?
    "${TKC}" "$1" --emit-llvm "${WORK}/p.tk" -o "${WORK}/p.ll" > "${WORK}/b" 2>&1; brc=$?
    cc=$(grep -o '"error_code":"[A-Z0-9]*"' "${WORK}/c" | sed 's/.*:"\(.*\)"/\1/' | sort -u | tr '\n' ' ')
    bc=$(grep -o '"error_code":"[A-Z0-9]*"' "${WORK}/b" | sed 's/.*:"\(.*\)"/\1/' | sort -u | tr '\n' ' ')
    echo "check=${crc} ${cc}| build=${brc} ${bc}"
}

expect "1 --max-funcs=1000000 (2.6 GB of FnSig): --check refuses as the build does" \
    "$(both --max-funcs=1000000)" "check=2 E9010 | build=2 E9010 "
expect "2 --max-structs=100000000: --check refuses as the build does" \
    "$(both --max-structs=100000000)" "check=2 E9010 | build=2 E9010 "
expect "3 --max-locals=100000000: --check refuses as the build does" \
    "$(both --max-locals=100000000)" "check=2 E9010 | build=2 E9010 "
expect "4 a negative cap: --check refuses as the build does" \
    "$(both --max-funcs=-1)" "check=2 E9010 | build=2 E9010 "
expect "5 --max-funcs=0 is a cap of 0 at --check too" \
    "$(both --max-funcs=0)" "check=1 E9010 | build=1 E9010 "
# the unallocatable-caps message names the caps, once
"${TKC}" --check --max-funcs=1000000 "${WORK}/p.tk" > "${WORK}/c" 2>&1
expect "6 --check's E9010 names the cap, once" \
    "$(grep -c 'max_funcs=1000000' "${WORK}/c")" "1"
expect "7 a large cap that fits passes both" \
    "$(both --max-funcs=4096)" "check=0 | build=0 "
expect "8 the default caps pass both" \
    "$(both --max-iters=0)" "check=0 | build=0 "

echo "C680: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
