#!/usr/bin/env bash
# C541_no_unreachable_sighup_reload.sh — server_ops.c carries no signal
# machinery that nothing installs, and metrics.c no empty reopen hook
# (story 121.51 items 3 and 4).
#
# THE DEFECT, measured on main (d3a083b8) + fix/r2-rsa:
#   server_ops.c defined proc_install_signals() (SIGHUP/SIGTERM/SIGUSR1/
#   SIGUSR2 handlers raising flags) and proc_check_reload(),
#   proc_check_shutdown(), proc_check_scale() to read them "from the parent
#   supervision loop".  Nothing called any of the four -- no stdlib module,
#   glue, header, .tki entry or toke-ooke source -- so SIGHUP never reached
#   the file, and the reload / log-rotation it documented did not exist.
#   metrics.c kept an empty metrics_reopen_logs() for a SIGHUP caller that
#   121.49 (1) removed, and C451 asserted it still linked.
#
# NOW: all of it is removed (git history holds it); the live SIGHUP path is
# http.c's worker respawn.  This compiles server_ops.c and metrics.c the way
# the stdlib does (-Werror) and reads their symbol tables.  Controls:
# server_ops.o still defines proc_healthz and metrics.o
# metrics_record_request, so an empty or wrong symbol table cannot pass.
#
# Negative control: C541_STDLIB=<dir> checks another src/stdlib tree; the
# pre-fix tree fails every removal check.
#
# Story: 121.51

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C541_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c541_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C541: no unreachable SIGHUP reload machinery, no empty reopen hook"
echo "------------------------------------------------------------------"

CF="-std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror"
for f in server_ops metrics; do
    # shellcheck disable=SC2086
    if ! "${CC}" ${CF} -iquote "${S}" -c "${S}/${f}.c" -o "${WORK}/${f}.o" > "${WORK}/${f}.cc" 2>&1; then
        fail "${f}.c does not compile: $(head -c 600 "${WORK}/${f}.cc")"
        echo ""; echo "C541: ${PASS} passed, ${FAIL} failed"; exit 1
    fi
    nm "${WORK}/${f}.o" > "${WORK}/${f}.nm" 2>&1
done

defines() { awk -v s="$2" '$2 ~ /^[TtDd]$/ && ($3 == s || $3 == "_" s) {f=1} END {exit !f}' "${WORK}/$1.nm"; }
refs()    { awk -v s="$2" '$1 == "U" && ($2 == s || $2 == "_" s) {f=1} END {exit !f}' "${WORK}/$1.nm"; }

defines server_ops proc_healthz && pass "control: server_ops.o defines proc_healthz" \
    || fail "control: proc_healthz not in server_ops.o's symbol table"
defines metrics metrics_record_request && pass "control: metrics.o defines metrics_record_request" \
    || fail "control: metrics_record_request not in metrics.o's symbol table"

for s in proc_install_signals proc_check_reload proc_check_shutdown proc_check_scale proc_signal_handler; do
    if defines server_ops "${s}"; then fail "server_ops.o still defines ${s}, which nothing calls"
    else pass "server_ops.o does not define ${s}"; fi
done
for s in sigaction signal; do
    if refs server_ops "${s}"; then fail "server_ops.o still installs signal handlers (${s})"
    else pass "server_ops.o does not reference ${s}"; fi
done
if defines metrics metrics_reopen_logs; then
    fail "metrics.o still defines the empty metrics_reopen_logs"
else
    pass "metrics.o does not define metrics_reopen_logs"
fi

# Nothing in the stdlib may still define or call a removed function.
calls="$(cd "${S}" && /usr/bin/grep -a -n -E '\b(proc_install_signals|proc_check_reload|proc_check_shutdown|proc_check_scale|metrics_reopen_logs)[[:space:]]*\(' -- *.c *.h 2>/dev/null \
    | /usr/bin/grep -a -v -E '^[^:]+:[0-9]+:[[:space:]]*(/?\*|//)' | head -5)"
if [ -n "${calls}" ]; then
    fail "the stdlib still names a removed function: ${calls}"
else
    pass "no stdlib source names a removed function outside a comment"
fi

echo ""
echo "C541: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
