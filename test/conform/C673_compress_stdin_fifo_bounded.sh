#!/usr/bin/env bash
# C673_compress_stdin_fifo_bounded.sh -- tkc --compress / --decompress /
# --compress-stream read standard input through tk_read_stream, so a FIFO or
# pipe on stdin cannot hang them (story 127.263).
#
# THE DEFECT.  127.254 measured that on Darwin under load the kernel sometimes
# loses the wakeup for a FIFO writer's close, leaving a reader asleep in a
# blocking read(2) at a recorded end-of-file -- for good.  tk_read_stream
# (src/readall.h) now reads a FIFO/pipe non-blockingly with a bounded
# select(), but the driver's --compress/--decompress/--compress-stream modes
# read stdin with their own blocking fgetc(stdin) loop and stayed exposed.
#
# THE FIX.  main.c reads stdin for those modes with tk_read_stream.
#
# WHAT IS ASSERTED (every tkc run bounded; a hang is a FAIL, never a stall)
#   1. --compress, --decompress and --compress-stream fed from a FIFO produce
#      byte-for-byte what they produce from a regular file on stdin;
#   2. an empty FIFO (the writer opens and closes at once) is read as empty;
#   3. STRESS, the 127.254 shape: 80 FIFO-fed --compress runs, four at a time,
#      each writer closing the moment it has written -- every run finishes,
#      bounded, with the expected output.
# Case 3 is probabilistic: main's fgetc loop hangs there only on a loaded
# machine (C210's note on 127.254 has the measured rate).  Cases 1-2 pin the
# output so the read path cannot change what the modes produce.
#
# Story: 127.263
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c673_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C673: --compress/--decompress read a FIFO on stdin, bounded (127.263)"
echo "---------------------------------------------------------------------"

# run_bounded SECS OUT IN CMD...: CMD < IN > OUT; 124 if it overran.
run_bounded() {
    local secs="$1" out="$2" in="$3"; shift 3
    "$@" < "${in}" > "${out}" 2>/dev/null & local p=$! t=0
    while kill -0 "${p}" 2>/dev/null; do
        if [ "${t}" -ge $((secs * 10)) ]; then
            kill -9 "${p}" 2>/dev/null; wait "${p}" 2>/dev/null; return 124
        fi
        sleep 0.1; t=$((t + 1))
    done
    wait "${p}"
}
# The writer blocks in open(2) until tkc's stdin redirection opens the FIFO.
# If tkc never gets that far it is killed, never waited on unbounded.
reap() {
    local w="$1" t=0
    while kill -0 "${w}" 2>/dev/null && [ "${t}" -lt 20 ]; do sleep 0.1; t=$((t + 1)); done
    kill "${w}" 2>/dev/null; wait "${w}" 2>/dev/null
}

printf '{"id":1,"name":"alpha","tags":["x","y"]}\n' > in.json
printf 'name,qty\nwidget,3\ngadget,5\n' > in.csv
printf 'the quick brown fox jumps over the lazy dog\nand again\n' > in.txt

# ── 1: FIFO output == regular-file output ─────────────────────────────────
for c in "--compress|in.json" "--compress|in.csv" "--compress|in.txt" \
         "--compress-stream|in.txt"; do
    mode="${c%%|*}"; src="${c#*|}"
    "${TKC}" "${mode}" < "${src}" > want 2>/dev/null
    rm -f s.fifo; mkfifo s.fifo
    (cat "${src}" > s.fifo) & w=$!
    rc=0; run_bounded 30 got s.fifo "${TKC}" "${mode}" || rc=$?
    reap "${w}"
    if [ "${rc}" -eq 0 ] && cmp -s want got && [ -s want ]; then
        pass "1 ${mode} ${src} from a FIFO: same bytes as from a file"
    else
        fail "1 ${mode} ${src} from a FIFO: rc=${rc} (124 = hung), outputs $(cmp -s want got && echo equal || echo differ)"
    fi
done
"${TKC}" --compress < in.txt > comp.txt 2>/dev/null
"${TKC}" --decompress < comp.txt > want 2>/dev/null
rm -f s.fifo; mkfifo s.fifo
(cat comp.txt > s.fifo) & w=$!
rc=0; run_bounded 30 got s.fifo "${TKC}" --decompress || rc=$?
reap "${w}"
if [ "${rc}" -eq 0 ] && cmp -s want got; then
    pass "1 --decompress from a FIFO: same bytes as from a file"
else
    fail "1 --decompress from a FIFO: rc=${rc}"
fi

# ── 2: empty FIFO ─────────────────────────────────────────────────────────
rm -f s.fifo; mkfifo s.fifo
(: > s.fifo) & w=$!
rc=0; run_bounded 30 got s.fifo "${TKC}" --compress || rc=$?
reap "${w}"
if [ "${rc}" -eq 0 ]; then pass "2 empty FIFO: exit 0"; else fail "2 empty FIFO: rc=${rc}"; fi

# ── 3: stress ─────────────────────────────────────────────────────────────
"${TKC}" --compress < in.json > want.json 2>/dev/null
batch() {   # batch DIR N: N sequential FIFO-fed runs; prints the failures
    local d="$1" n="$2" i w bad=0
    mkdir -p "${d}"
    for i in $(seq 1 "${n}"); do
        rm -f "${d}/f"; mkfifo "${d}/f"
        (cat in.json > "${d}/f") & w=$!
        run_bounded "${C673_TIMEOUT:-30}" "${d}/out" "${d}/f" "${TKC}" --compress \
            && cmp -s want.json "${d}/out" || bad=$((bad + 1))
        kill "${w}" 2>/dev/null; wait "${w}" 2>/dev/null
    done
    echo "${bad}"
}
for b in 1 2 3 4; do batch "b${b}" 20 > "bad${b}" & done; wait
bad=0; for b in 1 2 3 4; do bad=$((bad + $(cat "bad${b}" 2>/dev/null || echo 20))); done
if [ "${bad}" -eq 0 ]; then
    pass "3 80 FIFO-fed --compress runs under concurrency: all bounded and correct"
else
    fail "3 ${bad} of 80 FIFO-fed --compress runs failed or hung"
fi

echo "---------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
