# conform_timing.sh -- the time limits of the test/conform/*.sh cases (127.559).
#
# Sourced, never run:   . "${REPO_ROOT}/test/conform_timing.sh"
# (it lives beside run_test.sh, not in test/conform/, because every
# test/conform/*.sh is run as a case).
#
# An alarm in a case is a HANG GUARD: it must be reached only by a hang, never
# by a slow host.  Under host load 60-250 a compile has taken over 60 s and a
# quick program over 20 s, and an alarm sized for an idle host turned that
# into "build-failed", a missing binary or a wrong result further down
# (127.558, 127.559).  So the guards are long, one knob per kind, and a guard
# that fires says so by name:
#
#   TK_BUILD_SECS  TK_CONFORM_BUILD_SECS, default 600: a tkc build or cc compile
#   TK_RUN_SECS    TK_CONFORM_RUN_SECS,   default 120: one run of a quick
#                  program, or of tkc --check / --lint / --fmt / --min
#
# What a case proves must never depend on these values: a case that needs a
# time-based fact proves it from the program's own report (its diagnostic,
# its exit code), not from how long the guard let it run.

TK_BUILD_SECS="${TK_CONFORM_BUILD_SECS:-600}"
TK_RUN_SECS="${TK_CONFORM_RUN_SECS:-120}"
TK_RUN_TEST="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/run_test.sh"

# tk_guard SECS KNOB CMD [ARGS...] -- run CMD under a SECS alarm; when the alarm
# kills it (rc 142, SIGALRM) say so on stderr.  Returns CMD's status.
tk_guard() {
    local secs="$1" knob="$2" rc=0; shift 2
    "${TK_RUN_TEST}" "${secs}" "$@" || rc=$?
    if [ "${rc}" -eq 142 ]; then
        echo "TIMEOUT: $(basename "$1") killed by the ${secs} s hang guard (${knob}; rc 142: a hang, not load)" >&2
    fi
    return "${rc}"
}
tk_build() { tk_guard "${TK_BUILD_SECS}" TK_CONFORM_BUILD_SECS "$@"; }
tk_run()   { tk_guard "${TK_RUN_SECS}"   TK_CONFORM_RUN_SECS   "$@"; }

# tk_rc RC -- an exit status for a report: "N", or for 142 "142 (timed out:
# killed by its hang guard)".
tk_rc() {
    if [ "$1" -eq 142 ]; then echo "142 (timed out: killed by its hang guard)"
    else echo "$1"; fi
}

# tk_build_failed RC -- the token a case prints in place of a result when its
# build did not succeed: "build-failed rc=N" or "build-timed-out-after-Ns".
tk_build_failed() {
    if [ "$1" -eq 142 ]; then echo "build-timed-out-after-${TK_BUILD_SECS}s (rc 142, TK_CONFORM_BUILD_SECS hang guard)"
    else echo "build-failed rc=$1"; fi
}

# ── readiness waits (127.565) ───────────────────────────────────────────────
# A case that starts a server waits for the EVENT -- the port accepting, the
# port file written, the server_listening line on stderr -- never for a time.
# A fixed "50 x 0.1 s, then go on" turned a server that started late under
# host load into a refused connect or a hang-guard kill further down
# (127.561's C1881 case 5, at load 247).  The wait ends one of three ways,
# and the two failures say so by name on stderr:
#
#   tk_wait_until PID CHECK [ARGS...]
#     0    CHECK succeeded (it is run every 0.1 s)
#     1    PID exited first: "NOT READY: pid N exited (rc R) before: CHECK";
#          its exit status is left in TK_WAIT_RC (this reaps PID if it is a
#          child of this shell, so a later `wait PID` would read 127)
#     142  CHECK still failed after TK_CONFORM_RUN_SECS (default 120) with
#          PID still running: "TIMEOUT: ... (rc 142: a hang, not load)";
#          PID is left running for the case's own cleanup to stop
#
# CHECK runs in this shell (a function is fine) and must not block: a probe
# that can wait carries its own short timeout.  Its stdout and stderr are
# discarded.  A wait on a toke program's STDOUT line in a file can never be
# a CHECK: stdout to a file is flushed only at exit, so such a line never
# appears while the server runs -- wait on the port, or on a stderr
# DIAGNOSTIC line (stderr is unbuffered), instead.
TK_WAIT_RC=""
tk_wait_until() {
    local pid="$1" end=$(( SECONDS + TK_RUN_SECS )); shift
    TK_WAIT_RC=""
    while :; do
        "$@" >/dev/null 2>&1 && return 0
        if ! kill -0 "${pid}" 2>/dev/null; then
            # it may have become ready in the moment before it exited
            "$@" >/dev/null 2>&1 && return 0
            TK_WAIT_RC=0; wait "${pid}" 2>/dev/null || TK_WAIT_RC=$?
            if [ "${TK_WAIT_RC}" -eq 127 ]; then
                echo "NOT READY: pid ${pid} exited (rc unknown: not a child of this shell) before: $*" >&2
            else
                echo "NOT READY: pid ${pid} exited (rc $(tk_rc "${TK_WAIT_RC}")) before: $*" >&2
            fi
            return 1
        fi
        if [ "${SECONDS}" -ge "${end}" ]; then
            echo "TIMEOUT: pid ${pid} still running but not ready after ${TK_RUN_SECS} s (TK_CONFORM_RUN_SECS hang guard; rc 142: a hang, not load): $*" >&2
            return 142
        fi
        sleep 0.1
    done
}

# Checks for tk_wait_until.
# tk_nonempty FILE...    every FILE exists and is non-empty (a port file a
#                        helper server writes, and flushes, once listening)
tk_nonempty() {
    local f
    for f in "$@"; do [ -s "${f}" ] || return 1; done
}
# tk_port_open PORT [HOST]  a TCP connect to HOST:PORT (default 127.0.0.1)
#                        is accepted.  Loopback refuses at once when nothing
#                        listens, so this does not block.
tk_port_open() {
    (exec 3<>"/dev/tcp/${2:-127.0.0.1}/$1") 2>/dev/null
}
# tk_listening LOG       LOG (a toke server's stderr) holds its
#                        "DIAGNOSTIC: kind=server_listening port=N" line
tk_listening() {
    grep -a -q '^DIAGNOSTIC: kind=server_listening port=[0-9][0-9]*$' "$1" 2>/dev/null
}
# tk_listening_port LOG  prints N from that line (nothing if there is none)
tk_listening_port() {
    sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "$1" 2>/dev/null | head -1
}
