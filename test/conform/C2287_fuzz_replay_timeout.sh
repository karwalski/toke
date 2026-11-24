#!/usr/bin/env bash
# C2287_fuzz_replay_timeout.sh -- the fuzz replay bounds every input in time,
# so a non-termination regression fails make fuzz-smoke instead of hanging it
# (story 127.545).
#
# THE DEFECT: make fuzz-smoke (run by make ci) replays test/fuzz/seeds and
# test/fuzz/corpus-adversarial through each harness.  The standalone driver
# (test/fuzz/standalone_main.c) had no time limit and libFuzzer -runs=0 kept
# its default -timeout=1200, so the 127.542 yaml and 127.543 toon inputs,
# before their fixes, made the standalone replay run forever and the
# libFuzzer replay take 20 minutes per input.  A regression of that kind
# would have wedged the gate rather than failed it.
#
# THE FIX: the standalone driver arms alarm(FUZZ_REPLAY_TIMEOUT) around each
# input (default 25 s, 0 = off); an input still running at the bound ends the
# run with exit 70 and "fuzz-replay: TIMEOUT after N s on <path>".  The
# Makefile passes FUZZ_REPLAY_TIMEOUT to the standalone driver and as
# -timeout= to libFuzzer, and its failure summary shows the TIMEOUT line.
#
# WHAT IS ASSERTED (a toy harness linked against standalone_main.c that spins
# forever on an input starting with 'H'):
#   1. FUZZ_REPLAY_TIMEOUT=1 on [ok-input, hang-input]: exit 70 within 10 s,
#      stderr names the hanging input, the ok input was replayed first.
#   2. the same inputs without the hang: exit 0, 2 inputs replayed.
#   3. FUZZ_REPLAY_TIMEOUT=abc: exit 1 with a reason, nothing replayed.
#   4. the Makefile's fuzz-smoke passes the bound to both engines.
#
# Negative control: C2287_MAIN=<file> links another standalone_main.c (the
# one on main eca5062f or earlier); case 1 is then killed by this script's
# own 20 s alarm (exit 142) and case 3 replays both inputs.
#
# Story: 127.545
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MAIN_C="${C2287_MAIN:-${REPO_ROOT}/test/fuzz/standalone_main.c}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2287_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
echo "C2287: the fuzz replay bounds every input in time (127.545)"
echo "----------------------------------------------------------------------"

cat > "${WORK}/h.c" <<'CEOF'
#include <stddef.h>
#include <stdint.h>
volatile unsigned long spin;
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) {
    if (n > 0 && d[0] == 'H') for (;;) spin++;
    return 0;
}
CEOF
if ! "${CC}" -std=c99 -D_GNU_SOURCE -O0 -o "${WORK}/h" "${WORK}/h.c" "${MAIN_C}" > "${WORK}/cc.log" 2>&1; then
    fail "toy harness does not build"; head -8 "${WORK}/cc.log" | sed 's/^/        /'
    echo "C2287: ${PASS} passed, ${FAIL} failed"; exit 1
fi
printf 'a' > "${WORK}/1_ok"
printf 'H' > "${WORK}/2_hang"
printf 'b' > "${WORK}/2_ok"

now() { perl -MTime::HiRes=time -e 'printf "%.3f", time'; }

# 1. a hanging input fails the run, by name, inside the bound
t0=$(now)
FUZZ_REPLAY_TIMEOUT=1 perl -e 'alarm 20; exec @ARGV' "${WORK}/h" "${WORK}/1_ok" "${WORK}/2_hang" \
    > "${WORK}/o1" 2> "${WORK}/e1"; rc=$?
t1=$(now); dt=$(perl -e "printf '%.2f', $t1 - $t0")
if [ "${rc}" -eq 70 ] && grep -q "fuzz-replay: TIMEOUT after 1 s on ${WORK}/2_hang" "${WORK}/e1" \
        && perl -e "exit !($t1 - $t0 < 10)"; then
    pass "1 hanging input: exit 70 after ${dt} s, stderr names it"
else
    fail "1 hanging input: exit ${rc} after ${dt} s (want 70 within 10 s; 142 = killed by the 20 s alarm)"
    sed 's/^/        /' "${WORK}/e1"
fi

# 2. inputs that return are unaffected
FUZZ_REPLAY_TIMEOUT=1 perl -e 'alarm 20; exec @ARGV' "${WORK}/h" "${WORK}/1_ok" "${WORK}/2_ok" \
    > "${WORK}/o2" 2> "${WORK}/e2"; rc=$?
if [ "${rc}" -eq 0 ] && grep -q "2 input(s) replayed" "${WORK}/o2"; then
    pass "2 inputs that return: exit 0, 2 replayed"
else
    fail "2 inputs that return: exit ${rc}"; sed 's/^/        /' "${WORK}/o2" "${WORK}/e2"
fi

# 3. a malformed bound is refused, not ignored
FUZZ_REPLAY_TIMEOUT=abc perl -e 'alarm 20; exec @ARGV' "${WORK}/h" "${WORK}/1_ok" \
    > "${WORK}/o3" 2> "${WORK}/e3"; rc=$?
if [ "${rc}" -eq 1 ] && grep -q "FUZZ_REPLAY_TIMEOUT must be" "${WORK}/e3" && ! grep -q "replayed" "${WORK}/o3"; then
    pass "3 FUZZ_REPLAY_TIMEOUT=abc: exit 1 with a reason, nothing replayed"
else
    fail "3 FUZZ_REPLAY_TIMEOUT=abc: exit ${rc}"; sed 's/^/        /' "${WORK}/o3" "${WORK}/e3"
fi

# 4. the gate passes the bound to both engines
MK="${REPO_ROOT}/Makefile"
if grep -q -- '-runs=0 -timeout=$(FUZZ_REPLAY_TIMEOUT)' "${MK}" \
        && grep -q 'FUZZ_REPLAY_TIMEOUT=$(FUZZ_REPLAY_TIMEOUT) $$b ' "${MK}" \
        && grep -q '^FUZZ_REPLAY_TIMEOUT ?= ' "${MK}"; then
    pass "4 fuzz-smoke passes FUZZ_REPLAY_TIMEOUT to libFuzzer (-timeout=) and to the standalone driver"
else
    fail "4 fuzz-smoke does not pass FUZZ_REPLAY_TIMEOUT to both engines"
fi

echo "----------------------------------------------------------------------"
echo "C2287: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
