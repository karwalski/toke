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
