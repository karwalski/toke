#!/usr/bin/env bash
# C2270_process_exit_failure_status.sh — process.exit(0) after a recorded
# failure exits 1, exactly as os.exit(0) does (story 127.532).
#
# THE DEFECT.  A failed test.eq / test.ok / test.assert* (or an http.servetls
# that could not start) is recorded in src/stdlib/tk_exitfail.h and turns the
# process's exit status into 1 when it exits (127.231 item 2, C257).  _exit()
# skips the atexit handler that does this, so tk_os_exit calls
# tk_exitfail_atexit() itself for a status of 0.  tk_process_exit_w
# (src/stdlib/process_glue.c) called _exit() directly: `test.eq(1;2);
# process.exit(0)` exited 0 with no exit_status line, while the same program
# with os.exit(0) exited 1.  Pre-existing on v3.2.0.
#
# WHAT IS ASSERTED (compiled toke programs):
#   1. no failure, process.exit(0)          -> exit 0, no exit_status line
#   2. test.eq(1;2), process.exit(0)        -> exit 1, one exit_status line
#   3. test.eq(1;2), process.exit(4)        -> exit 4, no exit_status line
#                                              (a non-zero status is kept,
#                                              as os.exit(4) keeps it)
#   4. parity: for statuses 0 and 4, with a failure, process.exit and os.exit
#      give the same exit status and the same exit_status lines
#   5. TK_MEM_STATS=1 (127.529): process.exit(0) after a failure prints the
#      tk-mem-stats line exactly once, as os.exit(0) does (Darwin only;
#      skipped where the line is absent from the os.exit run as well)
#
# Negative control: C2270_STDLIB=<dir> builds against another stdlib tree; a
# copy with the base's process_glue.c (816f1577) fails cases 2, 4 (status 0)
# and 5.  TKC=<v3.2.0 tkc> (~/tk/tmpwork/wt_toke320) fails cases 2 and 4
# (status 0) and skips 5 (no TK_MEM_STATS there).
#
# Story: 127.532
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
if [ -n "${C2270_STDLIB:-}" ]; then export TKC_STDLIB_DIR="${C2270_STDLIB}"
elif [ -z "${TKC_STDLIB_DIR:-}" ] && [ "${TKC}" = "${REPO_ROOT}/tkc" ]; then
    export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"; fi
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2270_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C2270: process.exit(0) after a recorded failure exits 1 (127.532)"
echo "--------------------------------------------------------------"

prog() {   # prog NAME BODY -> builds NAME_bin
    printf 'm=main;\ni=test:std.test;\ni=os:std.os;\ni=process:std.process;\nf=main():i64{\n%s\n};\n' "$2" > "$1.tk"
    if ! "${TKC}" --allow-all --out "$1_bin" "$1.tk" > "$1.log" 2>&1; then
        echo "  FAIL: $1 did not compile"; sed 's/^/        /' "$1.log" | head -4
        FAIL=$((FAIL + 1)); return 1
    fi
}
# run NAME -> sets RC, NEXIT (exit_status lines), LINE (the exit_status line), ERR
run() {
    "./$1_bin" > "$1.out" 2> "$1.err"; RC=$?
    NEXIT=$(grep -a -c 'kind=exit_status' "$1.err")
    LINE="$(grep -a 'kind=exit_status' "$1.err")"
    ERR="$(head -c 400 "$1.err")"
}
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"
         [ -n "${ERR}" ] && printf '%s\n' "${ERR}" | sed 's/^/        | /'
         FAIL=$((FAIL + 1)); fi
}

if prog clean '  test.eq(1;1);
  process.exit(0);
  <0'; then
    run clean; expect "no failure, process.exit(0): exit 0, no exit_status line" "${RC} ${NEXIT}" "0 0"
fi

if prog pfail0 '  test.eq(1;2);
  process.exit(0);
  <0'; then
    run pfail0
    expect "test.eq(1;2) then process.exit(0): exit 1, one exit_status line" "${RC} ${NEXIT}" "1 1"
    P0="${RC}|${LINE}"
fi

if prog pfail4 '  test.eq(1;2);
  process.exit(4);
  <0'; then
    run pfail4
    expect "test.eq(1;2) then process.exit(4): exit 4, no exit_status line" "${RC} ${NEXIT}" "4 0"
    P4="${RC}|${LINE}"
fi

for st in 0 4; do
    if prog ofail${st} "  test.eq(1;2);
  os.exit(${st});
  <0"; then
        run ofail${st}; O="${RC}|${LINE}"
        if [ "${st}" = 0 ]; then P="${P0:-unbuilt}"; else P="${P4:-unbuilt}"; fi
        expect "status ${st} after a failure: process.exit matches os.exit" "${P}" "${O}"
    fi
done

if [ -x ./pfail0_bin ] && [ -x ./ofail0_bin ]; then
    TK_MEM_STATS=1 ./ofail0_bin > /dev/null 2> om.err
    TK_MEM_STATS=1 ./pfail0_bin > /dev/null 2> pm.err; PRC=$?
    ON=$(grep -a -c '^tk-mem-stats:' om.err); PN=$(grep -a -c '^tk-mem-stats:' pm.err)
    if [ "${ON}" = 0 ]; then
        echo "  SKIP: TK_MEM_STATS prints no line for os.exit here either"
    else
        ERR="$(head -c 400 pm.err)"
        expect "TK_MEM_STATS=1: process.exit(0) after a failure reports once, exits 1 (as os.exit)" \
            "${PRC} ${ON} ${PN} $(grep -a -c 'kind=exit_status' pm.err)" "1 1 1 1"
    fi
fi

echo "--------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
