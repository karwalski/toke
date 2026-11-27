#!/usr/bin/env bash
# C585_compress_stream_oversize_token.sh — --compress-stream passes a token
# longer than its buffer through verbatim instead of hanging, and
# --decompress does not wrap a long back-reference index (story 121.28,
# audit-120 compiler-codegen COM-02 and COM-04).
#
# THE DEFECTS.
#   COM-02: compress_stream_feed held an incomplete word/placeholder until
#   more input arrived, but a token that filled the whole 4096-byte buffer
#   could never be completed -- nothing consumed, buffer full, loop forever.
#   Measured at main f605aec9: a 100000-letter line hung until killed.
#   COM-04: the @Rn index was accumulated in an int with no cap -- signed
#   overflow (UB) that in practice wrapped: `@R4294967297` expanded to
#   dictionary word 1.
#
# WHAT THIS DRIVES.  (1) a 100000-letter line and a 6000-character
# placeholder, under a hang guard (TK_CONFORM_RUN_SECS), must come out byte-identical (neither has
# an abbreviation), with a short word after them still abbreviated; (2) the
# normal short-line behaviour is unchanged; (3) --decompress of @R1,
# @R4294967297 and @R18446744073709551617 against a one-word dictionary
# expands only the first.
#
# Negative control: TKC=<main f605aec9's toke> — case 1 fails (killed by the
# alarm, rc 142) and case 3 fails ("zebra quux zebra zebra zebra end").
#
# Story: 121.28, 127.559

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c585_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C585: compress-stream oversize tokens, decompress index overflow"
echo "---------------------------------------------------------------"

# 1. Oversize word and placeholder, under an alarm.
python3 -c "print('A'*100000); print('\$ABC' + 'D'*6000 + '12 the')" > "${WORK}/big.txt"
rc=0
tk_run "${TKC}" --compress-stream \
    <"${WORK}/big.txt" >"${WORK}/big.out" 2>"${WORK}/big.err" || rc=$?
want="$(python3 -c "print('A'*100000); print('\$ABC' + 'D'*6000 + '12 ~t')")"
if [ "${rc}" = 0 ] && [ "$(cat "${WORK}/big.out")" = "${want}" ]; then
    pass "100000-letter word and 6000-char placeholder pass through verbatim"
else
    fail "oversize tokens: rc $(tk_rc "${rc}"), $(wc -c <"${WORK}/big.out") bytes out"
fi

# 2. Short lines unchanged.
out="$(printf 'x the information $AB12 end\n' | "${TKC}" --compress-stream 2>&1)"
if [ "${out}" = 'x ~t information $AB12 end' ]; then
    pass "short line: abbreviation and placeholder as before"
else
    fail "short line: [${out}]"
fi

# 3. Decompress back-reference index does not wrap.
out="$(printf 'TK:P zebra quux @R1 @R4294967297 @R18446744073709551617 end\n' |
       "${TKC}" --decompress 2>&1)"
if [ "${out}" = ' zebra quux zebra   end' ]; then
    pass "@R1 expands; @R4294967297 and @R18446744073709551617 do not"
else
    fail "decompress: [${out}]"
fi

echo ""
echo "C585: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
