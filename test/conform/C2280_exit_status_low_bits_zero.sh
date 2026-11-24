#!/usr/bin/env bash
# C2280_exit_status_low_bits_zero.sh — os.exit(N) / process.exit(N) where the
# low 8 bits of N are 0 (256, 512, -256, ...) keep a recorded failure, exactly
# as main returning N does (story 127.536).
#
# THE DEFECT.  A failed test.eq / test.ok / test.assert* is recorded in
# src/stdlib/tk_exitfail.h and turns the exit status into 1 at exit (127.231,
# C257).  os.exit and process.exit are _exit(), which skips the atexit
# handler, so tk_os_exit and tk_process_exit_w ran the handler themselves —
# but only for a status of exactly 0.  _exit() keeps only the low 8 bits, so
# `test.eq(1;2); os.exit(256)` exited 0 with no exit_status line, while main
# returning 256 exits 1 (the handler counts a main status with zero low bits
# as 0).  Pre-existing on v3.2.0.
#
# THE FIX.  Both exit paths call tk_exitfail_before_exit(status), which runs
# the handler when (status & 0xff) == 0 — main's rule.
#
# DECIDED (conservative, matching main's return): with NO recorded failure,
# a status whose low 8 bits are 0 is left to _exit() and exits 0, as main
# returning 256 does.  A status with non-zero low bits is unchanged (exit
# status = low 8 bits, no exit_status line).
#
# WHAT IS ASSERTED (compiled toke programs; "line" = DIAGNOSTIC
# kind=exit_status, "1 1 1" = exit 1, one line, the line says status=1):
#   1. failure, os.exit(256)                -> 1 1 1
#   2. failure, process.exit(256)           -> 1 1 1
#   3. failure, os.exit(512)                -> 1 1 1
#   4. failure, process.exit(0-256)         -> 1 1 1
#   5. failure, main returns 256            -> 1 1 1 (the reference rule)
#   6. no failure, os.exit(256) / process.exit(256) / main returns 256
#                                           -> exit 0, no line (all three)
#   7. failure, os.exit(257)                -> exit 1, no line (low bits 1,
#                                              kept as given, unchanged)
#   8. failure, process.exit(4)             -> exit 4, no line (unchanged)
#
# Negative control: TKC=<v3.2.0 tkc> (~/tk/tmpwork/wt_toke320) fails cases
# 1-4 (exit 0, no line); C2280_STDLIB=<dir> builds against another stdlib
# tree (a copy with the base's os.c, process_glue.c and tk_exitfail.h,
# 1949cfbb, fails cases 1-4).
#
# Story: 127.536
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
if [ -n "${C2280_STDLIB:-}" ]; then export TKC_STDLIB_DIR="${C2280_STDLIB}"
elif [ -z "${TKC_STDLIB_DIR:-}" ] && [ "${TKC}" = "${REPO_ROOT}/tkc" ]; then
    export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"; fi
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2280_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C2280: exit statuses with zero low 8 bits keep a recorded failure (127.536)"
echo "--------------------------------------------------------------"

prog() {   # prog NAME BODY -> builds NAME_bin
    printf 'm=main;\ni=test:std.test;\ni=os:std.os;\ni=process:std.process;\nf=main():i64{\n%s\n};\n' "$2" > "$1.tk"
    if ! "${TKC}" --allow-all --out "$1_bin" "$1.tk" > "$1.log" 2>&1; then
        echo "  FAIL: $1 did not compile"; sed 's/^/        /' "$1.log" | head -4
        FAIL=$((FAIL + 1)); return 1
    fi
}
# run NAME -> sets RC, NEXIT (exit_status lines), NST1 (lines saying status=1), ERR
run() {
    "./$1_bin" > "$1.out" 2> "$1.err"; RC=$?
    NEXIT=$(grep -a -c 'kind=exit_status' "$1.err")
    NST1=$(grep -a -c 'kind=exit_status status=1 ' "$1.err")
    ERR="$(head -c 400 "$1.err")"
}
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"
         [ -n "${ERR}" ] && printf '%s\n' "${ERR}" | sed 's/^/        | /'
         FAIL=$((FAIL + 1)); fi
}
case_() {  # case_ NAME LABEL BODY WANT  (WANT = "RC NEXIT NST1")
    if prog "$1" "$3"; then run "$1"; expect "$2" "${RC} ${NEXIT} ${NST1}" "$4"; fi
}

case_ os256   "failure, os.exit(256): exit 1, one exit_status line (status=1)" \
    '  test.eq(1;2);
  os.exit(256);
  <0' "1 1 1"
case_ pr256   "failure, process.exit(256): exit 1, one exit_status line (status=1)" \
    '  test.eq(1;2);
  process.exit(256);
  <0' "1 1 1"
case_ os512   "failure, os.exit(512): exit 1, one exit_status line (status=1)" \
    '  test.eq(1;2);
  os.exit(512);
  <0' "1 1 1"
case_ prneg   "failure, process.exit(0-256): exit 1, one exit_status line (status=1)" \
    '  test.eq(1;2);
  process.exit(0-256);
  <0' "1 1 1"
case_ main256 "failure, main returns 256: exit 1, one exit_status line (reference rule)" \
    '  test.eq(1;2);
  <256' "1 1 1"
case_ cos256  "no failure, os.exit(256): exit 0, no exit_status line" \
    '  test.eq(1;1);
  os.exit(256);
  <0' "0 0 0"
case_ cpr256  "no failure, process.exit(256): exit 0, no exit_status line" \
    '  test.eq(1;1);
  process.exit(256);
  <0' "0 0 0"
case_ cmain256 "no failure, main returns 256: exit 0, no exit_status line" \
    '  test.eq(1;1);
  <256' "0 0 0"
case_ os257   "failure, os.exit(257): exit 1 (low bits), no exit_status line" \
    '  test.eq(1;2);
  os.exit(257);
  <0' "1 0 0"
case_ pr4     "failure, process.exit(4): exit 4, no exit_status line" \
    '  test.eq(1;2);
  process.exit(4);
  <0' "4 0 0"

echo "--------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
