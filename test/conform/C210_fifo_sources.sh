#!/usr/bin/env bash
# C210_fifo_sources.sh — tkc reads a source, companion or multi-module input
# from a FIFO (a stream that cannot seek) without overflowing the heap.
#
# THE DEFECT (135.3j item 2), on main at c8575da: every reader sized its
# buffer with fseek(SEEK_END)/ftell/rewind.  On a FIFO ftell returns -1, so
# the reader did malloc((size_t)-1 + 1) == malloc(0) and then
# fread(buf, 1, (size_t)-1, f) into it: a heap overflow driven by the input.
# Measured on main: `tkc --check fifo` -> exit 133 (the _FORTIFY_SOURCE trap
# on the fread), the same for a default build and for --verify-companion
# reading its companion file from a FIFO.
#
# NOW: src/readall.h tk_read_stream() reads in chunks and never seeks; the
# driver (single- and multi-module), both companion readers and tkir_read use
# it.  Each case below feeds a FIFO and asserts the ordinary result.
#
# Negative control: TKC=<main's toke> — cases 1-4 fail (exit 133, no JSON).
#
# Story: 135.3j (item 2)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

# feed FIFO TEXT: create the FIFO and start a writer in the background.  The
# writer blocks until tkc opens the FIFO; `wait` reaps it afterwards.
feed() { rm -f "$1"; mkfifo "$1" || { echo "FAIL mkfifo"; exit 1; }; (printf '%s' "$2" > "$1") & }
codes() { awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }

echo "C210: sources read from a FIFO (135.3j item 2)"

# 1. --check on a valid program read from a FIFO.
feed src.fifo 'm=main;f=main():i64{<0};'
"${TKC}" --check src.fifo > out1 2>&1; rc=$?; wait
expect "1 --check valid source from a FIFO: exit" "${rc}" "0"

# 2. --check on an ill-typed program from a FIFO still reports E4031.
feed src.fifo 'm=main;f=main():i64{let x:bool=1;<0};'
got="$("${TKC}" --check src.fifo 2>&1 | codes)"; wait
expect "2 --check ill-typed source from a FIFO: codes" "${got}" "E4031 "

# 3. a default build from a FIFO produces a program that runs.
feed src.fifo 'm=main;i=io:std.io;f=main():i64{io.println("fifo ok");<0};'
"${TKC}" src.fifo -o prog > out3 2>&1; rc=$?; wait
if [ "${rc}" -eq 0 ] && [ -x prog ]; then got="$(./prog)"; else got="build rc=${rc}"; fi
expect "3 build from a FIFO, run" "${got}" "fifo ok"

# 4. --verify-companion reading the companion itself from a FIFO.
printf 'm=main;f=main():i64{<0};\n' > a.tk
"${TKC}" --companion a.tk > a.tkc.md 2>/dev/null
feed comp.fifo "$(cat a.tkc.md)"
got="$("${TKC}" --verify-companion comp.fifo 2>&1 | awk '{print $1; exit}')"; wait
expect "4 --verify-companion from a FIFO" "${got}" "MATCH:"

# 5. regular files are unaffected: an empty source is still a parse-level
#    result, not a read failure (tk_read_stream returns a zero-length buffer).
: > empty.tk
out="$("${TKC}" --check empty.tk 2>&1)"
case "${out}" in *"failed to read"*) got="read failure" ;; *) got="read ok" ;; esac
expect "5 empty regular file is read, not refused" "${got}" "read ok"

echo "C210: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
