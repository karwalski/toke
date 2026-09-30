#!/usr/bin/env bash
# C620_gate_queue_fifo.sh — the conformance gate's machine-wide slots are
# handed out in arrival order, and a slot whose owner is gone is released
# (127.247).
#
# THE DEFECT.  scripts/conform_isolated.sh retried a `mkdir` slot lock every
# 2 s with no arrival order: whoever polled first after a release won, and
# with ~10 batches on 2 slots a run waited more than 6 h.  A finished batch's
# orphaned run -- alive, but the agent that launched it gone -- kept its slot
# until killed by hand, because only a DEAD owner pid released a slot.
#
# WHAT IS ASSERTED.  Every case drives the real script in `exec` mode against
# a private TKC_GATE_LOCKDIR with TKC_GATE_SLOTS=1 and a stand-in compiler (a
# shell script answering --version), so it neither waits for nor disturbs
# the machine's real gate slots.
#   1. FIFO: a holder H, then six waiters started 0.4 s apart.  When H lets
#      go, they run in arrival order, and waiter i announces
#      "queue position i of i" on arrival.
#   2. A holder killed with SIGKILL: the waiter gets the slot.
#   3. A holder ORPHANED (its launching shell is killed, the run continues):
#      the waiter gets the slot and says it released it.  The orphan is not
#      killed by the script (this test kills it afterwards, by exact PID).
#   4. The same orphan started with TKC_GATE_DETACHED=1 keeps its slot: the
#      waiter gives up after TKC_GATE_WAIT_SECS.
#   5. A waiter killed while queued does not block the ones behind it.
#
# Negative control: C620_SCRIPT=<main's scripts/conform_isolated.sh at
# f605aec9>, measured twice: case 1 red both times (orders "H W4 W6" -- three
# waiters still unserved after 30 s -- and "H W4 W5 W2 W1 W3 W6"; no queue
# position printed), case 3 red both times (the waiter gave up: the orphan's
# live pid kept the slot); cases 2, 4 and 5 green (dead-pid reclaim already
# existed; the old script records no chain and has no queue to block).
#
# Story: 127.247
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GATE="${C620_SCRIPT:-${REPO_ROOT}/scripts/conform_isolated.sh}"
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c620_XXXXXX")"
PIDS=()   # every process this test started, killed by exact PID at exit

cleanup() {
    local p f
    for f in "${WORK}"/*.pid; do
        [ -f "${f}" ] && PIDS+=("$(cat "${f}")")
    done
    for p in "${PIDS[@]+"${PIDS[@]}"}"; do
        [ -n "${p}" ] && kill -9 "${p}" 2>/dev/null
    done
    rm -rf "${WORK}"
}
trap cleanup EXIT

ok()  { PASS=$((PASS+1)); echo "PASS: $1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL: $1"; }

# A stand-in compiler: the script copies it and runs --version, nothing else.
cat > "${WORK}/fake_tkc" <<'EOF'
#!/bin/sh
echo "tkc fake-for-C620"
EOF
chmod +x "${WORK}/fake_tkc"

unset TKC_GATE_SLOT_HELD TKC_GATE_DETACHED
export TKC="${WORK}/fake_tkc" TKC_GATE_SLOTS=1 TKC_GATE_POLL_SECS=0.2

# until_true SECS CMD... -> 0 once CMD succeeds, 1 after SECS seconds.
until_true() {
    local end=$((SECONDS + $1)); shift
    until "$@"; do
        [ "${SECONDS}" -ge "${end}" ] && return 1
        sleep 0.1
    done
}
has_line() { grep -qx "$2" "$1" 2>/dev/null; }
slot_taken() { [ -f "$1/slot.1/pid" ]; }
tickets() { ls "$1/queue.slot" 2>/dev/null | grep -c '^[0-9]'; }
ntickets_is() { [ "$(tickets "$1")" -eq "$2" ]; }

# hold LOCKDIR TAG -> a run that holds the slot until $WORK/release.TAG exists.
# Writes its command's pid to $WORK/TAG.pid; the script's pid goes to
# HOLD_PID.  Called directly, never as $(hold ...): a holder launched from a
# command-substitution subshell loses that parent at once and so is, by the
# rule under test, orphaned.
hold() {
    local ld="$1" tag="$2"
    TKC_GATE_LOCKDIR="${ld}" TKC_GATE_WAIT_SECS=60 bash "${GATE}" exec -- \
        sh -c 'echo $$ > "$1/$2.pid"; echo "$2" >> "$1/order"; while [ ! -f "$1/release.$2" ]; do sleep 0.1; done' \
        _ "${WORK}" "${tag}" > "${WORK}/${tag}.out" 2>&1 &
    HOLD_PID=$!; PIDS+=("${HOLD_PID}")
}

# ── 1. FIFO ────────────────────────────────────────────────────────────────
L1="${WORK}/locks1"
hold "${L1}" H
if until_true 10 has_line "${WORK}/order" H; then
    for i in 1 2 3 4 5 6; do
        TKC_GATE_LOCKDIR="${L1}" TKC_GATE_WAIT_SECS=60 bash "${GATE}" exec -- \
            sh -c 'echo "$2" >> "$1/order"' _ "${WORK}" "W${i}" > "${WORK}/W${i}.out" 2>&1 &
        PIDS+=("$!")
        until_true 5 ntickets_is "${L1}" "${i}" || true
        sleep 0.4
    done
    touch "${WORK}/release.H"
    until_true 30 has_line "${WORK}/order" W6 || true
    sleep 0.5
    got="$(tr '\n' ' ' < "${WORK}/order")"
    if [ "${got}" = "H W1 W2 W3 W4 W5 W6 " ]; then ok "case 1: six waiters served in arrival order"
    else bad "case 1: order was '${got}', expected 'H W1 W2 W3 W4 W5 W6 '"; fi
    posok=1
    for i in 1 2 3 4 5 6; do
        grep -q "queue position ${i} of ${i} " "${WORK}/W${i}.out" || { posok=0; echo "    W${i}: $(grep -m1 'waiting' "${WORK}/W${i}.out")"; }
    done
    if [ "${posok}" -eq 1 ]; then ok "case 1: each waiter announced its queue position on arrival"
    else bad "case 1: a waiter did not announce 'queue position i of i'"; fi
    if grep -q releasing "${WORK}"/W?.out; then bad "case 1: a live holder's slot was released"; grep releasing "${WORK}"/W?.out | sed "s/^/    /"
    else ok "case 1: no live holder's slot was released"; fi
else
    bad "case 1: the holder never took the slot"; cat "${WORK}/H.out"
fi

# ── 2. a killed holder ────────────────────────────────────────────────────
L2="${WORK}/locks2"
hold "${L2}" K; h2="${HOLD_PID}"
if until_true 10 slot_taken "${L2}"; then
    TKC_GATE_LOCKDIR="${L2}" TKC_GATE_WAIT_SECS=20 bash "${GATE}" exec -- true > "${WORK}/W2.out" 2>&1 &
    w=$!; PIDS+=("${w}")
    sleep 0.5
    kill -9 "${h2}" 2>/dev/null
    if until_true 20 sh -c "! kill -0 ${w} 2>/dev/null" && wait "${w}"; then ok "case 2: a SIGKILLed holder's slot went to the waiter"
    else bad "case 2: the waiter did not get the slot"; sed 's/^/    /' "${WORK}/W2.out"; fi
else bad "case 2: the holder never took the slot"; fi

# ── 3/4. an orphaned holder ───────────────────────────────────────────────
# orphan LOCKDIR TAG -> starts a holder through a launcher shell, then kills
# the launcher.  The holder (the gate script, now reparented) keeps running.
orphan() {
    local ld="$1" tag="$2" launcher
    bash -c 'TKC_GATE_LOCKDIR="$1" TKC_GATE_WAIT_SECS=60 bash "$2" exec -- \
                 sh -c "echo \$\$ > \"$3/$4.pid\"; while :; do sleep 0.2; done" > "$3/$4.out" 2>&1 &
             echo $! > "$3/$4.script.pid"; wait' _ "${ld}" "${GATE}" "${WORK}" "${tag}" &
    launcher=$!; PIDS+=("${launcher}")
    until_true 10 slot_taken "${ld}" || return 1
    kill -9 "${launcher}" 2>/dev/null; wait "${launcher}" 2>/dev/null
    PIDS+=("$(cat "${WORK}/${tag}.script.pid")")
    kill -0 "$(cat "${WORK}/${tag}.script.pid")" 2>/dev/null   # still running
}

L3="${WORK}/locks3"
if orphan "${L3}" O; then
    TKC_GATE_LOCKDIR="${L3}" TKC_GATE_WAIT_SECS=8 bash "${GATE}" exec -- true > "${WORK}/W3.out" 2>&1
    rc=$?
    if [ "${rc}" -eq 0 ] && grep -q "releasing slot.1" "${WORK}/W3.out"; then
        ok "case 3: an orphaned holder's slot was released to the waiter"
    else bad "case 3: waiter rc=${rc}"; sed 's/^/    /' "${WORK}/W3.out"; fi
    kill -0 "$(cat "${WORK}/O.script.pid")" 2>/dev/null && ok "case 3: the orphan itself was not killed" \
        || bad "case 3: the gate script killed the orphan"
else bad "case 3: could not set up the orphan"; fi

L4="${WORK}/locks4"
if TKC_GATE_DETACHED=1 orphan "${L4}" D; then
    TKC_GATE_LOCKDIR="${L4}" TKC_GATE_WAIT_SECS=4 bash "${GATE}" exec -- true > "${WORK}/W4.out" 2>&1
    rc=$?
    if [ "${rc}" -ne 0 ] && grep -q "gave up waiting" "${WORK}/W4.out"; then
        ok "case 4: a TKC_GATE_DETACHED=1 run keeps its slot after its launcher exits"
    else bad "case 4: waiter rc=${rc}"; sed 's/^/    /' "${WORK}/W4.out"; fi
else bad "case 4: could not set up the detached run"; fi

# ── 5. a waiter killed while queued ───────────────────────────────────────
L5="${WORK}/locks5"
hold "${L5}" Q
if until_true 10 slot_taken "${L5}"; then
    TKC_GATE_LOCKDIR="${L5}" TKC_GATE_WAIT_SECS=60 bash "${GATE}" exec -- true > "${WORK}/Wk.out" 2>&1 &
    wk=$!; PIDS+=("${wk}")
    until_true 5 ntickets_is "${L5}" 1 || true
    TKC_GATE_LOCKDIR="${L5}" TKC_GATE_WAIT_SECS=20 bash "${GATE}" exec -- true > "${WORK}/W5.out" 2>&1 &
    w5=$!; PIDS+=("${w5}")
    until_true 5 ntickets_is "${L5}" 2 || true
    kill -9 "${wk}" 2>/dev/null; wait "${wk}" 2>/dev/null
    touch "${WORK}/release.Q"
    if until_true 20 sh -c "! kill -0 ${w5} 2>/dev/null" && wait "${w5}"; then ok "case 5: a dead waiter's ticket did not block the queue"
    else bad "case 5: the second waiter did not get the slot"; sed 's/^/    /' "${WORK}/W5.out"; ls -R "${L5}" | sed 's/^/    /'; fi
else bad "case 5: the holder never took the slot"; fi

echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
