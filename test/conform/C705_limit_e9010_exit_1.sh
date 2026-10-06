#!/usr/bin/env bash
# C705 -- every limit E9010 exits 1 (a user error); only a true internal error
# exits 2 (story 127.264 (2), owner decision docs/owner-decisions-2026-10-01.md
# §2.14).
#
# Before: a cap exceeded before codegen (functions/structs/imports/locals
# counted by tkc_caps.h) exited 1, but the same E9010 found DURING codegen,
# or caps too large to allocate (llvm_caps_fit, --check and build, single-
# and multi-file), exited 2 -- the status the CLI documents as "internal
# compiler error".  main.c now notes whether an E9010 error was emitted and a
# failed codegen/link step exits 1 when one was, 2 otherwise.
#
# Negative control: TKC=<c7f376a7's toke> -- cases 1-5 fail (rc=2); 4b, 6-8
# pass there too (they guard what did not change).
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c705_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT

# "rc=N codes" for a tkc run; the output goes to ${WORK}/o
run() {
    "${TKC}" "$@" > "${WORK}/o" 2>&1; local rc=$?
    local codes
    codes=$(grep -o '"error_code":"[A-Z0-9]*"' "${WORK}/o" | sed 's/.*:"\(.*\)"/\1/' | sort -u | tr '\n' ' ')
    echo "rc=${rc} ${codes}"
}

printf 'm=q;f=main():i64{<0};\n' > "${WORK}/p.tk"
printf 'm=a;f=main():i64{<0};\n' > "${WORK}/a.tk"
printf 'm=b;f=h():i64{<1};\n'    > "${WORK}/b.tk"
# 127 lets + a match arm binding: --check's lower bound passes it, codegen
# needs 130 local slots and refuses it (the C586 arm_lower_bound shape).
python3 - "${WORK}/arm.tk" <<'PY'
import sys
lets = ''.join('let v%d=%d;' % (i, i) for i in range(127))
open(sys.argv[1], 'w').write(
    'm=c705;\nt=$ue{$bad:str};\nf=g(n:i64):i64!$ue{<n};\n'
    'f=main():i64{' + lets + ' let r=mt g(1) {$ok:x x; $err:e 0}; <r};\n')
PY

expect "1 unallocatable caps, single-file build" \
    "$(run --max-funcs=1000000 --out "${WORK}/p" "${WORK}/p.tk")" "rc=1 E9010 "
expect "2 unallocatable caps, --check" \
    "$(run --check --max-structs=100000000 "${WORK}/p.tk")" "rc=1 E9010 "
expect "3 unallocatable caps, multi-file link" \
    "$(run --max-funcs=1000000 --out "${WORK}/ab" "${WORK}/a.tk" "${WORK}/b.tk")" "rc=1 E9010 "
expect "4 a cap first met during codegen (build)" \
    "$(run --out "${WORK}/arm" "${WORK}/arm.tk")" "rc=1 E9010 "
expect "4b ... and it names the limit" \
    "$(grep -c 'needs at least 130 local-variable slots' "${WORK}/o")" "1"
expect "5 a cap first met during codegen (--emit-llvm)" \
    "$(run --emit-llvm "${WORK}/arm.tk" -o "${WORK}/arm.ll")" "rc=1 E9010 "
expect "6 a cap met before codegen still exits 1" \
    "$(run --max-funcs=0 --out "${WORK}/p" "${WORK}/p.tk")" "rc=1 E9010 "
# 7: a codegen failure that is NOT a limit stays an internal error (rc 2).
expect "7 an unwritable IR output is E9002, rc 2 (not a limit)" \
    "$(run --emit-llvm "${WORK}/p.tk" -o "${WORK}/no/such/dir/p.ll")" "rc=2 E9002 "
expect "8 within the caps the build succeeds" \
    "$(run --out "${WORK}/p" "${WORK}/p.tk")" "rc=0 "

echo "C705: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
