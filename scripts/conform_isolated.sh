#!/usr/bin/env bash
# conform_isolated.sh — run the conformance gates so that concurrent runs
# cannot interfere with each other.  127.156 / 136.65.
#
# Usage: bash scripts/conform_isolated.sh all|yaml|sh
#        bash scripts/conform_isolated.sh exec -- CMD [ARGS...]
#   all   test/run_conform.sh, then every test/conform/*.sh   (make conform)
#   yaml  test/run_conform.sh only                            (make conform-check)
#   sh    every test/conform/*.sh only                        (make conform-sh)
#   exec  run CMD under the same slot, pinned $TKC and private $TMPDIR
#         (136.58: make check-probes).  Exit status is CMD's.
#
# What was shared, and what each run now gets instead:
#
#   THE COMPILER.  Every suite ran ./tkc, a symlink that any `make` in the same
#   tree relinks.  A run now copies the binary once, up front, into a private
#   directory and exports TKC at that copy; every suite honours $TKC.  A
#   relink mid-run can no longer change the compiler under test or leave a
#   suite exec'ing a half-written file.
#
#   TEMP FILES.  Each run gets a private TMPDIR.  Suites already mktemp their
#   work dirs; the last fixed /tmp path (L001's error file) is gone.
#
#   THE MACHINE.  Two things a private directory cannot isolate:
#     - load-sensitive suites.  S005 and S006 assert a NEGATIVE over hundreds
#       of connections ("no leak", "never throttled").  On a starved machine a
#       slow accept reads as a throttle -- and a run too slow to reach the
#       limit could equally pass without proving anything.  They run under a
#       machine-wide SERIAL lock, so at most one of them runs at a time
#       across all worktrees.
#     - memory.  Concurrent full runs got processes killed (the fourth symptom
#       in 127.156), which leaves no diagnostic at all.  A run takes one of
#       TKC_GATE_SLOTS machine-wide slots (default 2) and waits for a free one
#       rather than starting.
#
# Locks live in TKC_GATE_LOCKDIR (default /tmp/tkc_gate_locks) -- deliberately
# NOT per run: they exist to be seen by other runs.
#
# 127.247 -- waiters are served in ARRIVAL ORDER.  Until then every waiter
# retried mkdir every 2 s and whoever happened to poll first won, so with ~10
# batches on 2 slots one run waited more than 6 h.  Now a waiter takes a
# numbered ticket in ${LOCKROOT}/queue.<lock>/ and only the holder of the
# lowest live ticket may take a slot; each waiter prints its queue position
# when it starts waiting and whenever it moves.
#
# ABANDONED LOCKS.  Every lock and ticket directory records `pid` (its owner)
# and `chain`: the owner and each of its ancestors up to (not including) pid
# 1, with their start times.  The owner counts as live only while every
# process in that chain still exists with the same start time, so:
#   - a killed run's slot is released (as before);
#   - an ORPHANED run -- still running, but the shell or agent that launched
#     it is gone (a finished batch's run held a slot until killed by hand) --
#     has its slot released too.  It is not killed: releasing only lets the
#     next waiter start.  Start times guard against PID reuse.
# A run launched to outlive its parent on purpose (nohup, disown, setsid) sets
# TKC_GATE_DETACHED=1 so that only its own pid is recorded.  A lock written by
# an older copy of this script has no `chain` and is judged by its pid alone.
#
# Knobs: TKC_GATE_SLOTS (2), TKC_GATE_WAIT_SECS (3600), TKC_GATE_POLL_SECS (2,
# may be fractional), TKC_GATE_LOCKDIR, TKC_GATE_DETACHED, and
# TKC_GATE_DEBUG=<file> (append each release's evidence: the lock's pid and
# chain and the process snapshot it was judged against).  C620 tests this.

set -uo pipefail

MODE="${1:-all}"
case "${MODE}" in
    all|yaml|sh) ;;
    exec) shift; [ "${1:-}" = "--" ] && shift
          [ "$#" -gt 0 ] || { echo "usage: $0 exec -- CMD [ARGS...]" >&2; exit 2; }
          CMD=("$@") ;;
    *) echo "usage: $0 all|yaml|sh | exec -- CMD..." >&2; exit 2 ;;
esac

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 1

LOCKROOT="${TKC_GATE_LOCKDIR:-/tmp/tkc_gate_locks}"
SLOTS="${TKC_GATE_SLOTS:-2}"
WAIT_MAX="${TKC_GATE_WAIT_SECS:-3600}"
POLL="${TKC_GATE_POLL_SECS:-2}"   # seconds between polls; may be fractional
# Suites that must not overlap with another copy of themselves, or with each
# other, anywhere on the machine.  Add to this list with a story number.
SERIAL_SUITES="S005_http_route_param_leak S006_http_no_builtin_rate_limit"

mkdir -p "${LOCKROOT}" || { echo "conform: cannot create ${LOCKROOT}" >&2; exit 1; }

HELD=()
TICKET=""
SNAP=""

# snapshot -> SNAP: one line "PID PPID LSTART" per process on the machine.
snapshot() { SNAP="$(ps -A -o pid= -o ppid= -o lstart= 2>/dev/null)"; }

# stamp DIR: record our pid, then our chain, in DIR.  The pid goes first: it
# shrinks the unstamped window (mkdir .. pid) to one echo, and until the chain
# lands (renamed into place whole) a reader judges the owner by its pid alone.
stamp() {
    local d="$1"
    echo "$$" > "${d}/pid"
    snapshot
    if [ "${TKC_GATE_DETACHED:-0}" = 1 ]; then
        printf '%s\n' "${SNAP}" | awk -v me="$$" '$1==me {print $1, $3, $4, $5, $6, $7}'
    else
        printf '%s\n' "${SNAP}" | awk -v me="$$" '
            { pp[$1] = $2; st[$1] = $3 " " $4 " " $5 " " $6 " " $7 }
            END { p = me; n = 0
                  while ((p in pp) && p > 1 && n++ < 64) { print p, st[p]; p = pp[p] } }'
    fi > "${d}/chain.$$" && mv -f "${d}/chain.$$" "${d}/chain"
}

# chain_ok DIR -> 0 if every process in DIR's chain is in SNAP, same start.
chain_ok() {
    printf '%s\n' "${SNAP}" | awk '
        NR == FNR { if (NF >= 7) live[$1] = $3 " " $4 " " $5 " " $6 " " $7; next }
        NF >= 6 { if (!($1 in live) || live[$1] != $2 " " $3 " " $4 " " $5 " " $6) gone = 1 }
        END { exit gone }' - "$1/chain"
}

# alive DIR -> 0 while DIR's owner is live (see ABANDONED LOCKS).  Uses SNAP,
# and takes a fresh one before judging an owner gone: DIR may be newer than
# SNAP (a waiter that arrived, or a slot taken, since it was taken).
alive() {
    local d="$1" pid
    pid="$(cat "${d}/pid" 2>/dev/null || true)"
    if [ -z "${pid}" ]; then
        # Created but not yet stamped.  Two minutes on, its owner died
        # between mkdir and the pid write.
        [ -z "$(find "${d}" -maxdepth 0 -mmin +2 2>/dev/null)" ]
        return
    fi
    kill -0 "${pid}" 2>/dev/null || return 1
    [ -s "${d}/chain" ] || return 0
    chain_ok "${d}" && return 0
    snapshot
    chain_ok "${d}" && return 0
    # Judged gone -- unless DIR changed under us (its owner releasing it, or a
    # new owner stamping it): then it is not abandoned, and the next poll
    # looks again.
    [ -s "${d}/chain" ] && [ "$(cat "${d}/pid" 2>/dev/null)" = "${pid}" ] || return 0
    return 1
}

# drop DIR: remove a lock or ticket by renaming it away first, so a reader
# sees the whole directory or none of it -- never one half-deleted (127.247:
# a waiter read a releasing lock whose chain was already unlinked as
# abandoned).
drop() {
    local g="${1}.gone.$$.${RANDOM}"
    mv "$1" "${g}" 2>/dev/null && rm -rf "${g}"
}

# The mutex serialises reclaim-and-take, so two waiters cannot both judge a
# slot abandoned, and both remove it, and both take it.  Held for
# milliseconds; one whose holder died is removed.
MUTEX="${LOCKROOT}/mutex"
mutex_on() {
    until mkdir "${MUTEX}" 2>/dev/null; do
        snapshot
        alive "${MUTEX}" || drop "${MUTEX}"
        sleep 0.2
    done
    echo "$$" > "${MUTEX}/pid"
}
mutex_off() { [ "$(cat "${MUTEX}/pid" 2>/dev/null)" = "$$" ] && drop "${MUTEX}"; return 0; }

# try_lock DIR -> 0 if we now own DIR (call under the mutex, SNAP fresh).  A
# lock is a directory; mkdir is atomic, so exactly one contender wins.
try_lock() {
    local d="$1"
    if [ -d "${d}" ] && ! alive "${d}"; then
        echo "conform: releasing ${d##*/}: its owner, pid $(cat "${d}/pid" 2>/dev/null || echo '?'), or the process that launched it is gone (127.247)"
        if [ -n "${TKC_GATE_DEBUG:-}" ]; then
            { echo "== $$ releasing ${d}"; ls -la "${d}"; cat "${d}/pid" "${d}/chain"; echo "-- snap"; printf '%s\n' "${SNAP}"; } >> "${TKC_GATE_DEBUG}" 2>&1
        fi
        drop "${d}"
    fi
    if mkdir "${d}" 2>/dev/null; then
        stamp "${d}"
        HELD+=("${d}")
        return 0
    fi
    return 1
}

release_all() {
    local d
    for d in "${HELD[@]+"${HELD[@]}"}"; do
        [ "$(cat "${d}/pid" 2>/dev/null)" = "$$" ] && drop "${d}"
    done
    HELD=()
    [ -n "${TICKET}" ] && drop "${TICKET}"
    TICKET=""
}

# take_ticket QDIR: the next number in QDIR -- one above the highest ticket
# present, so it is above every live waiter's; mkdir settles a tie.
take_ticket() {
    local q="$1" n t
    mkdir -p "${q}" || return 1
    while :; do
        n="$(ls "${q}" 2>/dev/null | grep -E '^[0-9]{10}$' | tail -1)"
        n=$(( 10#${n:-0} + 1 ))
        t="${q}/$(printf '%010d' "${n}")"
        if mkdir "${t}" 2>/dev/null; then
            TICKET="${t}"
            stamp "${t}"
            return 0
        fi
    done
}

# queue_pos QDIR -> "AHEAD TOTAL": live tickets before ours, and all live
# tickets.  Tickets whose owner is gone are removed.  Uses SNAP.
queue_pos() {
    local q="$1" t name mine="${TICKET##*/}" ahead=0 total=0
    for t in "${q}"/*; do
        name="${t##*/}"
        case "${name}" in [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) ;; *) continue ;; esac
        if [ "${name}" != "${mine}" ] && ! alive "${t}"; then drop "${t}"; continue; fi
        total=$((total + 1))
        [[ "${name}" < "${mine}" ]] && ahead=$((ahead + 1))
    done
    echo "${ahead} ${total}"
}

# wait_lock WHAT QNAME DIR... -> takes the first free DIR in arrival order
# among the waiters for QNAME, waiting up to WAIT_MAX seconds.
wait_lock() {
    local what="$1" q="${LOCKROOT}/queue.$2"; shift 2
    local start="${SECONDS}" last="" pos ahead total d
    take_ticket "${q}" || { echo "conform: cannot create ${q}" >&2; return 1; }
    while :; do
        snapshot
        pos="$(queue_pos "${q}")"; ahead="${pos% *}"; total="${pos#* }"
        if [ "${ahead}" -eq 0 ]; then
            mutex_on
            for d in "$@"; do
                if try_lock "${d}"; then
                    mutex_off
                    drop "${TICKET}"; TICKET=""
                    return 0
                fi
            done
            mutex_off
        fi
        if [ "${pos}" != "${last}" ]; then
            echo "conform: waiting for ${what} -- queue position $((ahead + 1)) of ${total} (held by pid(s): $(for d in "$@"; do cat "${d}/pid" 2>/dev/null; done | tr '\n' ' '))"
            last="${pos}"
        fi
        if [ $((SECONDS - start)) -ge "${WAIT_MAX}" ]; then
            echo "conform: gave up waiting ${WAIT_MAX}s for ${what}" >&2
            drop "${TICKET}"; TICKET=""
            return 1
        fi
        sleep "${POLL}"
    done
}

PRIV=""
cleanup() {
    release_all
    [ -n "${PRIV}" ] && rm -rf "${PRIV}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ── a run slot (nested invocations inherit the parent's) ──────────────────
if [ -z "${TKC_GATE_SLOT_HELD:-}" ]; then
    slot_dirs=()
    for i in $(seq 1 "${SLOTS}"); do slot_dirs+=("${LOCKROOT}/slot.${i}"); done
    wait_lock "a gate slot (${SLOTS} machine-wide; TKC_GATE_SLOTS)" slot "${slot_dirs[@]}" || exit 1
    export TKC_GATE_SLOT_HELD=1
fi

# ── a private compiler and temp root ──────────────────────────────────────
PRIV="$(mktemp -d "${TMPDIR:-/tmp}/tkc_conform_run.XXXXXX")" || exit 1
mkdir -p "${PRIV}/tmp"
SRC_TKC="${TKC:-${REPO_ROOT}/tkc}"
if [ ! -x "${SRC_TKC}" ]; then
    echo "conform: no compiler at ${SRC_TKC} (run 'make' first)" >&2
    exit 1
fi
cp -L "${SRC_TKC}" "${PRIV}/tkc" && chmod +x "${PRIV}/tkc" || {
    echo "conform: cannot copy ${SRC_TKC}" >&2; exit 1; }
"${PRIV}/tkc" --version >/dev/null 2>&1 || {
    echo "conform: pinned copy of ${SRC_TKC} does not run" >&2; exit 1; }
export TKC="${PRIV}/tkc"
export TMPDIR="${PRIV}/tmp"
echo "conform: pinned $(${TKC} --version 2>/dev/null) sha256 $(shasum -a 256 "${TKC}" | cut -c1-12) from ${SRC_TKC}"

rc=0

if [ "${MODE}" = exec ]; then
    "${CMD[@]}"
    exit $?
fi

# 127.131: a raw NUL in a gate log makes the whole log "binary" to grep, so
# a later `grep PASS` on it prints nothing (ugrep -I) or "Binary file
# matches" (BSD/GNU grep) -- a passing run reads as one with no PASS lines.
# L007's description did this to every log through `echo -e` until 127.131.
# Refuse such a log rather than let it reach a reader.
nul_bytes() { LC_ALL=C tr -dc '\000' < "$1" | wc -c | tr -d ' '; }

if [ "${MODE}" = all ] || [ "${MODE}" = yaml ]; then
    ylog="${PRIV}/yaml.log"
    bash test/run_conform.sh | tee "${ylog}" || rc=1
    n="$(nul_bytes "${ylog}")"
    if [ "${n}" -ne 0 ]; then
        echo "conform: FAIL -- the conformance output contains ${n} NUL byte(s); grep would read it as binary (127.131)"
        rc=1
    fi
fi

if [ "${MODE}" = all ] || [ "${MODE}" = sh ]; then
    # 127.166: a suite that cannot run because an optional external tool is
    # missing exits 0 and prints a line starting `CONFORM-SKIP: <reason>`.
    # It is counted and printed as SKIP -- neither a pass nor a failure --
    # unless TKC_CONFORM_NO_SKIP=1 (CI), where every skip is a failure.
    pass=0; fail=0; skip=0
    log="${PRIV}/suite.log"
    for s in test/conform/*.sh; do
        name="$(basename "${s}" .sh)"
        serial=0
        case " ${SERIAL_SUITES} " in *" ${name} "*) serial=1 ;; esac
        if [ "${serial}" -eq 1 ]; then
            wait_lock "the serial lock for ${name}" serial "${LOCKROOT}/serial" || { fail=$((fail+1)); echo "FAIL ${s}: serial lock not acquired"; continue; }
        fi
        if bash "${s}" > "${log}" 2>&1 && [ "$(nul_bytes "${log}")" -eq 0 ]; then
            why=""
            if [ -n "$(sed -n '/^CONFORM-SKIP:/=' "${log}")" ]; then
                why="$(sed -n 's/^CONFORM-SKIP: *//p' "${log}" | head -1)"
                [ -n "${why}" ] || why="(no reason given)"
            fi
            if [ -n "${why}" ] && [ "${TKC_CONFORM_NO_SKIP:-0}" = 1 ]; then
                fail=$((fail+1)); echo "FAIL ${s}: skipped, and TKC_CONFORM_NO_SKIP=1: ${why}"
            elif [ -n "${why}" ]; then
                skip=$((skip+1)); echo "SKIP ${s}: ${why}"
            else
                # 127.131: awk, not grep -- a log carrying one control byte is
                # "binary" to grep, which then prints "Binary file matches" (BSD)
                # or nothing at all (ugrep -I) instead of the line.
                pass=$((pass+1)); echo "PASS ${s}: $(awk '/^Results:/{l=$0} END{print l}' "${log}")"
            fi
        else
            fail=$((fail+1)); echo "FAIL ${s}"
            n="$(nul_bytes "${log}")"
            [ "${n}" -eq 0 ] || echo "    (its output contains ${n} NUL byte(s) -- binary to grep; 127.131)"
            tail -25 "${log}" | LC_ALL=C tr -d '\000' | sed 's/^/    /'
        fi
        if [ "${serial}" -eq 1 ]; then
            d="${LOCKROOT}/serial"
            [ "$(cat "${d}/pid" 2>/dev/null)" = "$$" ] && drop "${d}"
        fi
    done
    echo "conform-sh: ${pass} scripts passed, ${fail} failed, ${skip} skipped"
    [ "${fail}" -eq 0 ] || rc=1
    # 127.131: a sweep that ran nothing did not pass.
    if [ $((pass + fail + skip)) -eq 0 ]; then
        echo "conform-sh: NO suites ran -- that is not a pass" >&2; rc=1
    fi
fi

exit "${rc}"
