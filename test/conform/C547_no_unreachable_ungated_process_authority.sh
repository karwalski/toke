#!/usr/bin/env bash
# C547_no_unreachable_ungated_process_authority.sh — the unreachable, ungated
# process-authority functions of audit 120 AMB-09 / AMB-10 are gone
# (story 121.42 item 7).
#
# THE DEFECT, measured on main (d3a083b8):
#   AMB-09  process.c process_set_cwd() stored a directory only on a handle
#           not yet spawned, but process_spawn creates the handle and sets its
#           pid before returning -- so it was always a no-op, and spawn never
#           read the stored value.  No wrapper or .tki entry reached it.
#   AMB-10  server_ops.c proc_binary_upgrade() setenv'd TK_LISTEN_FD, cleared
#           FD_CLOEXEC and fork+execv'd a binary; proc_inherit_listen_fd()
#           trusted TK_LISTEN_FD from the environment.  Nothing called them
#           and no capability check guarded them.
#
# NOW: removed (git history holds them).  This compiles process.c and
# server_ops.c the way the stdlib does (-Werror) and reads their symbol
# tables.  Controls: process.o defines process_spawn, server_ops.o
# proc_healthz, so an empty or wrong symbol table cannot pass.
#
# Negative control: C547_STDLIB=<dir> checks another src/stdlib tree; main's
# fails every removal check.
#
# Story: 121.42
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C547_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c547_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
echo "C547: no unreachable, ungated process authority (AMB-09, AMB-10)"
echo "----------------------------------------------------------------"
CF="-std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror"
for f in process server_ops; do
    # shellcheck disable=SC2086
    if ! "${CC}" ${CF} -iquote "${S}" -c "${S}/${f}.c" -o "${WORK}/${f}.o" > "${WORK}/${f}.cc" 2>&1; then
        fail "${f}.c does not compile: $(head -c 600 "${WORK}/${f}.cc")"
        echo ""; echo "C547: ${PASS} passed, ${FAIL} failed"; exit 1
    fi
    nm "${WORK}/${f}.o" > "${WORK}/${f}.nm" 2>&1
done
defines() { awk -v s="$2" '$2 ~ /^[TtDd]$/ && ($3 == s || $3 == "_" s) {f=1} END {exit !f}' "${WORK}/$1.nm"; }
refs()    { awk -v s="$2" '$1 == "U" && ($2 == s || $2 == "_" s) {f=1} END {exit !f}' "${WORK}/$1.nm"; }

defines process process_spawn && pass "control: process.o defines process_spawn" \
    || fail "control: process_spawn not in process.o's symbol table"
defines server_ops proc_healthz && pass "control: server_ops.o defines proc_healthz" \
    || fail "control: proc_healthz not in server_ops.o's symbol table"
if defines process process_set_cwd; then fail "AMB-09: process.o still defines the no-op process_set_cwd"
else pass "AMB-09: process.o does not define process_set_cwd"; fi
for s in proc_binary_upgrade proc_inherit_listen_fd; do
    if defines server_ops "${s}"; then fail "AMB-10: server_ops.o still defines ${s}"
    else pass "AMB-10: server_ops.o does not define ${s}"; fi
done
for s in fork execv setenv unsetenv getenv; do
    if refs server_ops "${s}"; then fail "AMB-10: server_ops.o still calls ${s}"
    else pass "AMB-10: server_ops.o does not call ${s}"; fi
done
echo ""
echo "C547: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
