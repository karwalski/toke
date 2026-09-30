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
# 127.254: under load a FIFO read could hang for good -- case 4 sat 36 minutes
# in a conform run.  Darwin loses the wakeup for the writer's close now and
# then (about 1 read in 450 at load average 25, measured), leaving the reader
# asleep in read(2) at a recorded end-of-file.  tk_read_stream now reads a
# FIFO non-blockingly with a bounded select() between reads (src/readall.h).
# Every tkc call here is bounded (run_bounded, below): a hang is a FAIL with
# the case named, never a stalled gate.  The writer is reaped with a bound
# too: if tkc exits without opening the FIFO, the writer sits in open(2) and
# a bare `wait` would stall exactly as badly.  Case 6 reads 80 FIFOs, four at
# a time, each writer closing at once -- the shape that lost the wakeup.
# It is probabilistic: main's toke hangs there only on a loaded machine.
#
# Story: 135.3j (item 2), 127.254

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
# writer blocks until tkc opens the FIFO; reap() collects it afterwards.
WRITER=""
feed() { rm -f "$1"; mkfifo "$1" || { echo "FAIL mkfifo"; exit 1; }; (printf '%s' "$2" > "$1") & WRITER=$!; }
# reap: the writer finishes as soon as tkc has opened the FIFO.  If tkc never
# opened it the writer is still blocked in open(2): kill it, don't wait on it.
reap() {
    [ -n "${WRITER}" ] || return 0
    local t=0
    while kill -0 "${WRITER}" 2>/dev/null && [ "${t}" -lt 20 ]; do sleep 0.1; t=$((t + 1)); done
    kill "${WRITER}" 2>/dev/null; wait "${WRITER}" 2>/dev/null; WRITER=""
}
# run_bounded OUT CMD...: run CMD with stdout+stderr to OUT, at most
# C210_TIMEOUT seconds (portable: no GNU timeout on macOS).  Returns CMD's
# status, or 124 after killing a CMD that overran.
C210_TIMEOUT="${C210_TIMEOUT:-60}"
run_bounded() {
    local out="$1"; shift
    "$@" > "${out}" 2>&1 & local p=$! t=0
    while kill -0 "${p}" 2>/dev/null; do
        if [ "${t}" -ge $((C210_TIMEOUT * 10)) ]; then
            kill -9 "${p}" 2>/dev/null; wait "${p}" 2>/dev/null
            echo "TIMEOUT after ${C210_TIMEOUT}s: $*" >> "${out}"; return 124
        fi
        sleep 0.1; t=$((t + 1))
    done
    wait "${p}"
}
codes() { awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }

echo "C210: sources read from a FIFO (135.3j item 2)"

# 1. --check on a valid program read from a FIFO.
feed src.fifo 'm=main;f=main():i64{<0};'
run_bounded out1 "${TKC}" --check src.fifo; rc=$?; reap
expect "1 --check valid source from a FIFO: exit" "${rc}" "0"

# 2. --check on an ill-typed program from a FIFO still reports E4031.
feed src.fifo 'm=main;f=main():i64{let x:bool=1;<0};'
run_bounded out2 "${TKC}" --check src.fifo; rc=$?; reap
if [ "${rc}" -eq 124 ]; then got="hang (${C210_TIMEOUT}s)"; else got="$(codes < out2)"; fi
expect "2 --check ill-typed source from a FIFO: codes" "${got}" "E4031 "

# 3. a default build from a FIFO produces a program that runs.
feed src.fifo 'm=main;i=io:std.io;f=main():i64{io.println("fifo ok");<0};'
run_bounded out3 "${TKC}" src.fifo -o prog; rc=$?; reap
if [ "${rc}" -eq 0 ] && [ -x prog ]; then got="$(./prog)"; else got="build rc=${rc}"; fi
expect "3 build from a FIFO, run" "${got}" "fifo ok"

# 4. --verify-companion reading the companion itself from a FIFO.
printf 'm=main;f=main():i64{<0};\n' > a.tk
run_bounded a.tkc.md sh -c '"$0" --companion a.tk 2>/dev/null' "${TKC}"
feed comp.fifo "$(cat a.tkc.md)"
run_bounded out4 "${TKC}" --verify-companion comp.fifo; rc=$?; reap
if [ "${rc}" -eq 124 ]; then got="hang (${C210_TIMEOUT}s)"; else got="$(awk '{print $1; exit}' out4)"; fi
expect "4 --verify-companion from a FIFO" "${got}" "MATCH:"

# 5. regular files are unaffected: an empty source is still a parse-level
#    result, not a read failure (tk_read_stream returns a zero-length buffer).
: > empty.tk
run_bounded out5 "${TKC}" --check empty.tk; out="$(cat out5)"
case "${out}" in *"failed to read"*) got="read failure" ;; *) got="read ok" ;; esac
expect "5 empty regular file is read, not refused" "${got}" "read ok"

# 6. 127.254: 80 FIFO reads, four at a time, each writer closing the moment
#    it has written.  Every read must finish, bounded, and verify.
fifo_batch() {   # fifo_batch DIR N: N sequential FIFO verifies; prints fails
    local d="$1" n="$2" i w bad=0
    mkdir -p "${d}" && cp a.tk a.tkc.md "${d}/" || { echo "${n}"; return; }
    for i in $(seq 1 "${n}"); do
        rm -f "${d}/c.fifo"; mkfifo "${d}/c.fifo"
        (printf '%s' "$(cat "${d}/a.tkc.md")" > "${d}/c.fifo") & w=$!
        run_bounded "${d}/out" "${TKC}" --verify-companion "${d}/c.fifo"
        grep -q '^MATCH' "${d}/out" || bad=$((bad + 1))
        kill "${w}" 2>/dev/null; wait "${w}" 2>/dev/null
    done
    echo "${bad}"
}
C210_TIMEOUT_SAVED="${C210_TIMEOUT}"; C210_TIMEOUT="${C210_BATCH_TIMEOUT:-30}"
for b in 1 2 3 4; do fifo_batch "b${b}" 20 > "bad${b}" & done; wait
C210_TIMEOUT="${C210_TIMEOUT_SAVED}"
bad=0; for b in 1 2 3 4; do bad=$((bad + $(cat "bad${b}" 2>/dev/null || echo 20))); done
expect "6 80 FIFO reads under concurrency: failed or hung" "${bad}" "0"

echo "C210: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
