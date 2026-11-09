#!/usr/bin/env bash
# C1201_tls_groups_every_context.sh — every SSL_CTX the stdlib creates gets
# the shared hybrid-first group list, and there is exactly one copy of the
# list-setting code (story 122.15).
#
# C1200 drives the contexts that exist today end to end.  This guard keeps
# the property for contexts added later: a new SSL_CTX_new() in src/stdlib/
# without a tk_tls_set_groups() call in the same function would be hybrid
# only by OpenSSL's default again, which is the defect 122.15 removed.
#
#   1. In every src/stdlib/*.c, each function that calls SSL_CTX_new( also
#      calls tk_tls_set_groups( -- checked per function, not per file, so a
#      second context in an already-covered file is still caught.
#   2. The two SSL_CTX_set1_groups_list calls naming TK_TLS_GROUPS_* appear
#      in tk_tls_groups.h and nowhere else (one helper, no copies).
#   3. Nothing in src/stdlib sets a group list by literal string outside the
#      helper (a caller-configured list -- tls.curves -- passes a variable).
#
# Static and fast; no OpenSSL needed.
#
# Story: 122.15

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
S="${C1201_STDLIB:-${REPO_ROOT}/src/stdlib}"

echo "C1201: every stdlib SSL_CTX gets tk_tls_set_groups (one shared helper)"
echo "-----------------------------------------------------------------------"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# 1. per function: a top-level function starts at a line beginning with an
#    identifier/type and containing '(' and ends at a line that is exactly
#    '}'.  Report each function holding SSL_CTX_new( without the helper.
REPORT="$(for f in "${S}"/*.c; do
    awk -v F="$(basename "$f")" '
        /^[A-Za-z_].*\(/ && !/;[[:space:]]*$/ { fn = $0; has_new = 0; has_set = 0; nline = 0 }
        /SSL_CTX_new\(/  { has_new = 1; if (!nline) nline = NR }
        /tk_tls_set_groups\(/ { has_set = 1 }
        /^}/ {
            if (has_new) printf "%s %s:%d %s\n", (has_set ? "ok" : "MISSING"), F, nline, fn
            has_new = 0; has_set = 0; nline = 0
        }' "$f"
done)"
NOK="$(printf '%s\n' "${REPORT}" | grep -c '^ok ' | tr -d ' ')"
NMISS="$(printf '%s\n' "${REPORT}" | grep -c '^MISSING ' | tr -d ' ')"
NALL="$(grep -h -c 'SSL_CTX_new(' "${S}"/*.c | awk '{s += $1} END {print s}')"
if [ "${NMISS}" = 0 ] && [ "${NOK}" -ge 6 ]; then
    pass "${NOK} functions create an SSL_CTX and every one calls tk_tls_set_groups"
else
    fail "functions creating an SSL_CTX without tk_tls_set_groups: ${NMISS} (covered: ${NOK})"
    printf '%s\n' "${REPORT}" | grep '^MISSING ' | sed 's/^/        /'
fi
# the per-function scan must have seen every SSL_CTX_new( line; a function
# creating two contexts is counted once, so compare against the line count
# with that allowance made explicit
NSEEN="$(for f in "${S}"/*.c; do
    awk '/^[A-Za-z_].*\(/ && !/;[[:space:]]*$/ { inf = 1 } /SSL_CTX_new\(/ && inf { n++ } /^}/ { inf = 0 } END { print n + 0 }' "$f"
done | awk '{s += $1} END {print s}')"
if [ "${NSEEN}" = "${NALL}" ] && [ "${NALL}" -gt 0 ]; then
    pass "the scan attributed all ${NALL} SSL_CTX_new( calls to a function"
else
    fail "the scan saw ${NSEEN} of ${NALL} SSL_CTX_new( calls inside a function -- the guard is blind to some"
fi

# 2. one copy of the list-setting code
COPIES="$(grep -n 'SSL_CTX_set1_groups_list([^,]*, *TK_TLS_GROUPS_' "${S}"/*.c "${S}"/*.h | sed "s|${S}/||")"
if [ "$(printf '%s\n' "${COPIES}" | grep -c .)" = 2 ] && \
   [ "$(printf '%s\n' "${COPIES}" | grep -c '^tk_tls_groups.h:')" = 2 ]; then
    pass "TK_TLS_GROUPS_HYBRID / _CLASSICAL are applied only inside tk_tls_groups.h"
else
    fail "group-list calls outside the shared helper:"; printf '%s\n' "${COPIES}" | sed 's/^/        /'
fi

# 3. no literal group list anywhere else
LIT="$(grep -n -E 'SSL_(CTX_)?set1_(groups|curves)_list\([^,]*, *"' "${S}"/*.c "${S}"/*.h | sed "s|${S}/||")"
if [ -z "${LIT}" ]; then
    pass "no stdlib source sets a group list by literal string"
else
    fail "literal group lists outside the helper:"; printf '%s\n' "${LIT}" | sed 's/^/        /'
fi

echo ""
echo "C1201: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
