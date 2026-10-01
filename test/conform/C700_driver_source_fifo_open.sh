#!/usr/bin/env bash
# C700_driver_source_fifo_open.sh -- tkc never blocks in open(2) on a source
# that is a FIFO with no writer (story 121.85).
#
# THE DEFECT.  The driver opened sources with fopen(src) and then read them
# with tk_read_stream.  open(2) of a FIFO that no process has open for
# writing blocks until a writer arrives -- forever if none does -- before
# tk_read_stream's bounded non-blocking loop (127.254) runs.  Both the
# single-module path and the multi-module path did this.
#
# THE FIX.  main.c tk_read_source opens O_NONBLOCK and, for a FIFO, waits at
# most 10 s for a writer (deciding with a one-byte non-blocking read), then
# refuses the source with "is a FIFO that no process opened for writing"
# (exit 3, usage).  A writer already blocked in its open, or one that arrives
# within the bound, is read as before.
#
# WHAT IS ASSERTED (each tkc run bounded at 40 s; 124 = hung and killed)
#   1. --check on a writerless FIFO: exits 3 (usage) within the bound with the
#      FIFO message;
#   2. multi-module build with a writerless FIFO as one of the sources: same;
#   3. a writer already waiting in its open(2): the source is read and checks
#      clean (exit 0);
#   4. a writer that opens the FIFO 1 s AFTER tkc starts: read, exit 0;
#   5. a writer whose first byte arrives 1 s after it opens: read, exit 0
#      (the one-byte probe keeps no byte back: the source must be whole);
#   6. a regular file: exit 0.
#
# Negative control: TKC=<main's toke> -- cases 1-2 hang (124).
#
# Story: 121.85
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c700_XXXXXX")"
WRITERS=""
cleanup() {
    for w in ${WRITERS}; do kill "${w}" 2>/dev/null; wait "${w}" 2>/dev/null; done
    rm -rf "${WORK}"
}
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C700: tkc does not block in open(2) on a writerless FIFO source (121.85)"
echo "------------------------------------------------------------"

run_bounded() {   # run_bounded SECS OUT CMD...: stdout+stderr to OUT; 124 if killed
    local secs="$1" out="$2"; shift 2
    "$@" > "${out}" 2>&1 & local p=$! t=0
    while kill -0 "${p}" 2>/dev/null; do
        if [ "${t}" -ge $((secs * 10)) ]; then
            kill -9 "${p}" 2>/dev/null; wait "${p}" 2>/dev/null; return 124
        fi
        sleep 0.1; t=$((t + 1))
    done
    wait "${p}"
}

SRC='m=a;
f=main():i64{<0};'
printf '%s\n' "${SRC}" > good.tk
printf 'm=b;\nf=two():i64{<2};\n' > b.tk

# ── 1: single source, writerless FIFO ─────────────────────────────────────
mkfifo nw.tk
rc=0; run_bounded 40 o1 "${TKC}" --check nw.tk || rc=$?
if [ "${rc}" -eq 3 ] && grep -q "no process opened for writing" o1; then
    pass "1 --check on a writerless FIFO: exit 3, FIFO named"
else
    fail "1 --check on a writerless FIFO: rc=${rc} (124 = hung) '$(head -c 200 o1)'"
fi

# ── 2: multi-module, one writerless FIFO ──────────────────────────────────
rc=0; run_bounded 40 o2 "${TKC}" good.tk nw.tk --out m2 || rc=$?
if [ "${rc}" -eq 3 ] && grep -q "nw.tk' is a FIFO that no process opened for writing" o2; then
    pass "2 multi-module build with a writerless FIFO: exit 3, FIFO named"
else
    fail "2 multi-module build with a writerless FIFO: rc=${rc} (124 = hung) '$(head -c 200 o2)'"
fi

# ── 3: writer already blocked in its open ─────────────────────────────────
rm -f w3.tk; mkfifo w3.tk
(printf '%s\n' "${SRC}" > w3.tk) & WRITERS="${WRITERS} $!"
sleep 0.3   # let it reach open(2)
rc=0; run_bounded 40 o3 "${TKC}" --check w3.tk || rc=$?
if [ "${rc}" -eq 0 ]; then pass "3 writer waiting in open: read, exit 0"
else fail "3 writer waiting in open: rc=${rc} '$(head -c 200 o3)'"; fi

# ── 4: writer opens 1 s after tkc starts ──────────────────────────────────
rm -f w4.tk; mkfifo w4.tk
(sleep 1; printf '%s\n' "${SRC}" > w4.tk) & WRITERS="${WRITERS} $!"
rc=0; run_bounded 40 o4 "${TKC}" --check w4.tk || rc=$?
if [ "${rc}" -eq 0 ]; then pass "4 writer arriving 1 s late: read, exit 0"
else fail "4 writer arriving 1 s late: rc=${rc} '$(head -c 200 o4)'"; fi

# ── 5: writer open, first byte 1 s later ──────────────────────────────────
rm -f w5.tk; mkfifo w5.tk
(exec 3>w5.tk; sleep 1; printf '%s\n' "${SRC}" >&3; exec 3>&-) & WRITERS="${WRITERS} $!"
rc=0; run_bounded 40 o5 "${TKC}" --check w5.tk || rc=$?
if [ "${rc}" -eq 0 ]; then pass "5 writer open, data 1 s later: read whole, exit 0"
else fail "5 writer open, data 1 s later: rc=${rc} '$(head -c 200 o5)'"; fi

# ── 6: regular file ───────────────────────────────────────────────────────
rc=0; run_bounded 40 o6 "${TKC}" --check good.tk || rc=$?
if [ "${rc}" -eq 0 ]; then pass "6 regular file: exit 0"
else fail "6 regular file: rc=${rc} '$(head -c 200 o6)'"; fi

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
