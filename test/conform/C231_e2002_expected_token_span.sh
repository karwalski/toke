#!/usr/bin/env bash
# C231_e2002_expected_token_span.sh — an "expected X, got 'Y'" E2002 (the
# parser's xp() site) carries the offending token's span: text mode
# underlines the whole token and JSON span_end covers it (story 123.13 item 4).
#
# Before: that emit site used diag_emit (no span), so text mode drew a single
# '^' under the first character of 'barbaz' and JSON reported
# span_start == span_end, while every other token-level parse error
# (eerr, eerr_got, E2003) was span-routed.
#
# Negative control: put xp()'s E2002 back on diag_emit -> both cases fail
# (the caret line holds a single '^' and span_end == span_start).
#
# 127.233: the record no longer carries a fix, so nothing follows the
# underline on the caret line; the match accepts end of line as well as a
# trailing space.  The property asserted -- all six characters underlined --
# is unchanged.
#
# Story: 123.13
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d /tmp/tkc_c231_XXXXXX)"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C231: E2002 'expected X, got Y' spans the offending token (123.13)"

printf 'm=main;\nf=main():i64{\n  <0\n};\nf=foo(x:i64 barbaz):i64{<x};\n' > e.tk

"${TKC}" --check --diag-text e.tk > text.out 2>&1
# the first diagnostic is the xp() E2002 at 'barbaz' (col 13, 6 chars)
caret="$(awk '/^error\[E2002\]: expected .\)., got .barbaz./{f=1} f&&/\^/{print; exit}' text.out)"
if printf '%s\n' "${caret}" | grep -Eq '\|             \^~~~~~( |$)'; then
    echo "  PASS: text mode underlines all of 'barbaz'"; PASS=$((PASS+1))
else
    echo "  FAIL: text mode caret line: '${caret}'"; FAIL=$((FAIL+1))
fi

"${TKC}" --check --diag-json e.tk > json.out 2>&1
rec="$(grep -m1 '"message":"expected .)., got .barbaz.' json.out)"
if printf '%s' "${rec}" | grep -q '"span_start":42,"span_end":48'; then
    echo "  PASS: JSON span covers the token (42..48)"; PASS=$((PASS+1))
else
    echo "  FAIL: JSON record: ${rec:0:300}"; FAIL=$((FAIL+1))
fi

echo "C231: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
