#!/usr/bin/env bash
# C257_test_failure_exit_status.sh — a failed std.test assertion makes the
# program exit non-zero (story 127.231 item 2).
#
# THE DEFECT.  test.eq / test.assert* printed a DIAGNOSTIC line on failure and
# returned false, and nothing else happened: unless the program called
# test.report, it returned 0 from main and exited 0, so a test program could
# not fail through its exit status.  test.ok did less — it was still the
# `val ? 1 : 0` stub and printed nothing at all.
#
# DECIDED: AT EXIT.  A failure is recorded (src/stdlib/tk_exitfail.h) and the
# program keeps running, so every failure is still reported; when the process
# exits, it exits 1 and says why in one DIAGNOSTIC kind=exit_status line.
# os.exit(0) keeps the failure too.  A program that returns some other
# non-zero status after a failure exits 1 (documented in tk_exitfail.h).
#
# WHAT IS ASSERTED (compiled toke programs):
#   1. all assertions pass                 -> exit 0, no exit_status line
#   2. test.eq fails (int and str)         -> both DIAGNOSTICs, exit 1, one
#                                             exit_status line with failures=2
#   3. test.ok(0)                          -> a DIAGNOSTIC, exit 1
#   4. test.assert(false) then os.exit(0)  -> exit 1
#   5. a failure, then main returns 3      -> exit 1 (the recorded status)
#
# Negative control: C257_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's tk_test.c, test_glue.c and os.c fails cases 2-5.
#
# Story: 127.231
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C257_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c257_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C257: a failed std.test assertion fails the process (127.231)"
echo "--------------------------------------------------------------"

prog() {   # prog NAME BODY -> builds NAME_bin
    printf 'm=main;\ni=test:std.test;\ni=os:std.os;\nf=main():i64{\n%s\n};\n' "$2" > "$1.tk"
    if ! "${TKC}" --allow-all --out "$1_bin" "$1.tk" > "$1.log" 2>&1; then
        echo "  FAIL: $1 did not compile"; sed 's/^/        /' "$1.log" | head -4
        FAIL=$((FAIL + 1)); return 1
    fi
}
# run NAME -> sets RC, NDIAG (assertion_failed lines), NEXIT (exit_status lines), ERR
run() {
    "./$1_bin" > "$1.out" 2> "$1.err"; RC=$?
    NDIAG=$(grep -a -c 'kind=assertion_failed' "$1.err")
    NEXIT=$(grep -a -c 'kind=exit_status status=1' "$1.err")
    ERR="$(head -c 400 "$1.err")"
}
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"
         [ -n "${ERR}" ] && printf '%s\n' "${ERR}" | sed 's/^/        | /'
         FAIL=$((FAIL + 1)); fi
}

if prog pass '  test.eq(1;1);
  test.eq("a";"a");
  test.ok(1);
  test.assert(true;"true is true");
  <0'; then
    run pass; expect "all assertions pass: exit 0, no exit_status line" "${RC} ${NDIAG} ${NEXIT}" "0 0 0"
fi

if prog eqfail '  test.eq(1;2);
  test.eq("a";"b");
  <0'; then
    run eqfail
    expect "two failed test.eq: both reported, exit 1, one exit_status line" "${RC} ${NDIAG} ${NEXIT}" "1 2 1"
    expect "the exit_status line counts both failures" \
        "$(grep -a -c 'kind=exit_status status=1 failures=2 ' eqfail.err)" "1"
fi

if prog okfail '  test.ok(0);
  <0'; then
    run okfail; expect "test.ok(0) reports a failure and exits 1" "${RC} ${NDIAG} ${NEXIT}" "1 1 1"
fi

if prog osexit '  test.assert(false;"planned failure");
  os.exit(0);
  <0'; then
    run osexit; expect "a failure then os.exit(0) exits 1" "${RC} ${NEXIT}" "1 1"
fi

if prog ret3 '  test.eq(1;2);
  <3'; then
    run ret3; expect "a failure then main returning 3 exits 1 (the recorded status)" "${RC}" "1"
fi

echo "--------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
