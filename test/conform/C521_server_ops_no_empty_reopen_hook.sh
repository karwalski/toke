#!/usr/bin/env bash
# C521_server_ops_no_empty_reopen_hook.sh — server_ops.c no longer calls
# metrics.c's empty metrics_reopen_logs() (story 121.49 item 1).
#
# THE DEFECT, measured on main (7014e929):
#   server_ops.c defined proc_reopen_logs(), documented as "reopen all log
#   file descriptors ... called during SIGHUP reload", whose whole body was a
#   call to metrics_reopen_logs() -- a function 121.41 left empty when it
#   deleted metrics.c's log files.  Nothing called proc_reopen_logs either
#   (no stdlib module, glue, header or .tki entry), so the "log rotation"
#   entry point did nothing and promised otherwise.
#
# NOW: the call, its extern and the uncalled wrapper are gone.  This builds
# server_ops.c the way the stdlib builds it (-Werror) and reads its symbol
# table: no undefined reference to metrics_reopen_logs, no proc_reopen_logs
# definition.  Control: the object still defines proc_healthz, so an empty
# or wrong symbol table cannot pass.
#
# Negative control: C521_SERVER_OPS_C=<file> checks another server_ops.c;
# main's fails both checks.
#
# Story: 121.49

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
SRC="${C521_SERVER_OPS_C:-${S}/server_ops.c}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c521_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C521: server_ops.c has no empty log-reopen hook"
echo "-----------------------------------------------"

if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -iquote "${S}" \
        -c "${SRC}" -o "${WORK}/so.o" > "${WORK}/cc.log" 2>&1; then
    fail "server_ops.c does not compile: $(head -c 600 "${WORK}/cc.log")"
    echo ""; echo "C521: ${PASS} passed, ${FAIL} failed"; exit 1
fi
nm "${WORK}/so.o" > "${WORK}/nm.txt" 2>&1

if awk '$2 == "T" && $3 ~ /^_?proc_healthz$/ {f=1} END {exit !f}' "${WORK}/nm.txt"; then
    pass "control: server_ops.o defines proc_healthz"
else
    fail "control: proc_healthz not found in server_ops.o's symbol table"
fi
if awk '$1 == "U" && $2 ~ /^_?metrics_reopen_logs$/ {f=1} END {exit !f}' "${WORK}/nm.txt"; then
    fail "server_ops.o still references metrics_reopen_logs (an empty function)"
else
    pass "server_ops.o does not reference metrics_reopen_logs"
fi
if awk '$3 ~ /^_?proc_reopen_logs$/ {f=1} END {exit !f}' "${WORK}/nm.txt"; then
    fail "server_ops.o still defines proc_reopen_logs, which reopens nothing and has no caller"
else
    pass "server_ops.o does not define proc_reopen_logs"
fi

echo ""
echo "C521: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
