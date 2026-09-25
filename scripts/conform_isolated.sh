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
# NOT per run: they exist to be seen by other runs.  A lock whose owner PID is
# gone is reclaimed, so a killed run cannot wedge the next one.

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
# Suites that must not overlap with another copy of themselves, or with each
# other, anywhere on the machine.  Add to this list with a story number.
SERIAL_SUITES="S005_http_route_param_leak S006_http_no_builtin_rate_limit"

mkdir -p "${LOCKROOT}" || { echo "conform: cannot create ${LOCKROOT}" >&2; exit 1; }

HELD=()

# try_lock DIR -> 0 if we now own DIR.  A lock is a directory holding the
# owner's PID; mkdir is atomic, so exactly one contender wins.
try_lock() {
    local d="$1" pid
    if mkdir "${d}" 2>/dev/null; then
        echo "$$" > "${d}/pid"
        HELD+=("${d}")
        return 0
    fi
    pid="$(cat "${d}/pid" 2>/dev/null || true)"
    if [ -n "${pid}" ] && ! kill -0 "${pid}" 2>/dev/null; then
        # Owner is gone: reclaim.  rm then mkdir, so a racing reclaimer
        # simply loses the mkdir.
        rm -rf "${d}"
        if mkdir "${d}" 2>/dev/null; then
            echo "$$" > "${d}/pid"
            HELD+=("${d}")
            return 0
        fi
    elif [ -z "${pid}" ] && [ -n "$(find "${d}" -maxdepth 0 -mmin +2 2>/dev/null)" ]; then
        # Created but never stamped, two minutes ago: its owner died between
        # mkdir and the pid write.
        rm -rf "${d}"
    fi
    return 1
}

release_all() {
    local d
    for d in "${HELD[@]+"${HELD[@]}"}"; do
        [ "$(cat "${d}/pid" 2>/dev/null)" = "$$" ] && rm -rf "${d}"
    done
    HELD=()
}

# wait_lock WHAT DIR... -> takes the first free DIR, waiting up to WAIT_MAX.
wait_lock() {
    local what="$1"; shift
    local waited=0 announced=0 d
    while :; do
        for d in "$@"; do
            try_lock "${d}" && return 0
        done
        if [ "${announced}" -eq 0 ]; then
            echo "conform: waiting for ${what} (held by pid(s): $(for d in "$@"; do cat "${d}/pid" 2>/dev/null; done | tr '\n' ' '))"
            announced=1
        fi
        if [ "${waited}" -ge "${WAIT_MAX}" ]; then
            echo "conform: gave up waiting ${WAIT_MAX}s for ${what}" >&2
            return 1
        fi
        sleep 2; waited=$((waited + 2))
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
    wait_lock "a gate slot (${SLOTS} machine-wide; TKC_GATE_SLOTS)" "${slot_dirs[@]}" || exit 1
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

if [ "${MODE}" = all ] || [ "${MODE}" = yaml ]; then
    bash test/run_conform.sh || rc=1
fi

if [ "${MODE}" = all ] || [ "${MODE}" = sh ]; then
    pass=0; fail=0
    log="${PRIV}/suite.log"
    for s in test/conform/*.sh; do
        name="$(basename "${s}" .sh)"
        serial=0
        case " ${SERIAL_SUITES} " in *" ${name} "*) serial=1 ;; esac
        if [ "${serial}" -eq 1 ]; then
            wait_lock "the serial lock for ${name}" "${LOCKROOT}/serial" || { fail=$((fail+1)); echo "FAIL ${s}: serial lock not acquired"; continue; }
        fi
        if bash "${s}" > "${log}" 2>&1; then
            pass=$((pass+1)); echo "PASS ${s}: $(grep -E '^Results:' "${log}" | tail -1)"
        else
            fail=$((fail+1)); echo "FAIL ${s}"; tail -25 "${log}" | sed 's/^/    /'
        fi
        if [ "${serial}" -eq 1 ]; then
            d="${LOCKROOT}/serial"
            [ "$(cat "${d}/pid" 2>/dev/null)" = "$$" ] && rm -rf "${d}"
        fi
    done
    echo "conform-sh: ${pass} scripts passed, ${fail} failed"
    [ "${fail}" -eq 0 ] || rc=1
fi

exit "${rc}"
