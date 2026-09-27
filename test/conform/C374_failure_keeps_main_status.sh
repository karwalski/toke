#!/usr/bin/env bash
# C374_failure_keeps_main_status.sh — after a recorded failure (a failed
# std.test assertion) the process exits with main's own status when main
# returned a non-zero one, and 1 otherwise (135.29, Batch GB2).
#
# THE DEFECT.  tk_exitfail.h's atexit handler ended every failed run with
# _exit(1): an atexit handler cannot read the status exit() was given, so a
# program that returned 3 after a failed assertion exited 1.  The emitted C
# `main` (src/llvm.c) now stores the truncated tk_main result in
# @tk_exitfail_main_status before its `ret` — a STRONG definition in the
# entry module overriding the header's weak one, the capabilities.c
# pattern — and the handler exits with it when its low 8 bits are non-zero.
#
# WHAT IS ASSERTED (compiled toke programs; "rc status-line"):
#   1. failure, main returns 3    -> 3, "status=3"
#   2. failure, main returns 0    -> 1, "status=1"
#   3. failure, main returns 256  -> 1 (256 would exit 0), "status=1"
#   4. failure, then os.exit(0)   -> 1 (main never returned)
#   5. no failure, main returns 5 -> 5, no status line (control)
#   6. the entry module's IR defines the global and main stores to it
#
# Negative control: TKC + stdlib from main at 9f901bf — cases 1 and 6 red
# ("1 status=1", "0 0"); 2-5 green on both.  Injection (the emitted store
# removed) -> 1 and 6 red.  Mixed builds link on macOS both ways (this tkc
# with main's stdlib, main's tkc with this stdlib): each then exits 1, the
# old behaviour, never a link error.
#
# Story: 135.29

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C374_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# run BODY -> "rc status=N|none"
run() {
    printf 'm=main;i=test:std.test;i=os:std.os;\nf=main():i64{\n%s\n};\n' "$1" > "${WORK}/p.tk"
    rm -f "${WORK}/b"
    "${TKC}" --allow-all "${WORK}/p.tk" --out "${WORK}/b" >/dev/null 2>&1 || { echo build-failed; return; }
    "${WORK}/b" >/dev/null 2>"${WORK}/err"; local rc=$?
    local st; st=$(sed -n 's/^DIAGNOSTIC: kind=exit_status \(status=[0-9]*\).*/\1/p' "${WORK}/err" | head -1)
    echo "${rc} ${st:-none}"
}

expect "1-failure-main-3"    "$(run '  test.eq(1;2);
  <3')" "3 status=3"
expect "2-failure-main-0"    "$(run '  test.eq(1;2);
  <0')" "1 status=1"
expect "3-failure-main-256"  "$(run '  test.eq(1;2);
  <256')" "1 status=1"
expect "4-failure-os-exit-0" "$(run '  test.eq(1;2);
  os.exit(0);
  <3')" "1 status=1"
expect "5-no-failure-main-5" "$(run '  test.eq(2;2);
  <5')" "5 none"

printf 'm=main;\nf=main():i64{<7};\n' > "${WORK}/q.tk"
"${TKC}" --emit-llvm "${WORK}/q.tk" -o "${WORK}/q.ll" >/dev/null 2>&1
expect "6-ir-publishes-status" \
    "$(grep -c '^@tk_exitfail_main_status = global i32 0$' "${WORK}/q.ll") $(grep -c 'store volatile i32 %rc, i32\* @tk_exitfail_main_status' "${WORK}/q.ll")" "1 1"

echo "C374: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
