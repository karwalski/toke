#!/usr/bin/env bash
# C335_e2002_no_generic_fix.sh — a generic E2002 carries no `fix` field, and
# still carries its `got` field (story 127.233).
#
# THE DEFECT.  The parser's two generic E2002 sites -- xp() ("expected ')',
# got 'barbaz'") and eerr_got() ("unexpected token in expression, got ')'")
# -- put "fix":"check syntax; see toke spec for valid constructs" on every
# record.  That is advice, not a fix: nothing in it can be applied, so it is
# not "deterministically correct in every case the error is emitted"
# (AGENTS §6), and docs/spec/errors.md already lists E2002's fix as absent.
#
# THE TRAP IN THE FIX.  diag.c's key/value scan stops at the first NULL
# value.  eerr_got listed "fix" before "got", so once E2002's fix is NULL the
# "got" field silently vanished with it.  "got" is now listed first.
#
# WHAT IS ASSERTED, per site (JSON records whose code is E2002):
#   1. no record has a "fix" key
#   2. the first record's "got" is the offending token
#   3. xp site only: "expected" is the token it wanted
#
# Negative controls (run 2026-09-27):
#   - TKC=<main 512f904 build>: 1 fails at both sites (fix present).
#   - put "fix" back ahead of "got" in eerr_got: 2 fails at the eerr_got site
#     ("got" is "").
#
# D107/D108 pin fix absence through the D-series runner; this script adds
# the "got" check the runner cannot express.
#
# Story: 127.233
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c335_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C335: generic E2002 has no fix field and keeps its got field (127.233)"
echo "------------------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# e2002 FILE : "<records with a fix key>|<first record's got>|<first record's expected>"
e2002() { python3 -c '
import json,sys
recs=[]
for l in open(sys.argv[1]):
    l=l.strip()
    if not l.startswith("{"): continue
    d=json.loads(l)
    if d.get("error_code")=="E2002": recs.append(d)
if not recs: print("NO-E2002"); sys.exit(0)
print("%d|%s|%s" % (sum(1 for d in recs if "fix" in d), recs[0].get("got","<absent>"), recs[0].get("expected","<absent>")))' "$1"; }

# xp() site
printf 'm=test;\nf=foo(x:i64 barbaz):i64{<x};\n' > xp.tk
"${TKC}" --check --diag-json xp.tk > xp.log 2>&1; rc=$?
R="$(e2002 xp.log)"
expect "xp: exit 1" "${rc}" "1"
expect "xp: no E2002 record has a fix key" "${R%%|*}" "0"
expect "xp: got is the offending token, expected is ')'" "${R#*|}" "barbaz|')'"

# eerr_got() site
printf 'm=test;\nf=main():i64{<)};\n' > eg.tk
"${TKC}" --check --diag-json eg.tk > eg.log 2>&1; rc=$?
R="$(e2002 eg.log)"
expect "eerr_got: exit 1" "${rc}" "1"
expect "eerr_got: no E2002 record has a fix key" "${R%%|*}" "0"
expect "eerr_got: got is the offending token" "$(printf '%s' "${R}" | cut -d'|' -f2)" ")"

echo ""
echo "C335: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
