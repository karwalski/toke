#!/usr/bin/env bash
# C350_e2004_e2005_no_advice_fix.sh — E2004 and E2005 carry no `fix` field,
# and keep their `got` field (story 127.235).
#
# THE DEFECT.  Three parser sites put advice in the `fix` field:
#   - xp() at end of file, E2004: "add the missing closing delimiter"
#   - eerr(), E2004 "unclosed delimiter": the same text
#   - eerr_got(), E2005: "expected a type: scalar ($i64, ...), struct ..."
# None of these can be applied: the parser knows a delimiter is missing but
# not where it belongs, and a list of type forms is not a type.  So none is
# "deterministically correct in every case the error is emitted" (AGENTS §6).
# Same class as E2002's generic fix (127.233, C335).
#
# THE TRAP IN THE FIX.  diag.c's key/value scan stops at the first NULL
# value, so a NULL fix listed ahead of "got" drops "got" as well.
#
# WHAT IS ASSERTED:
#   1. no E2004 / E2005 record has a "fix" key (both E2004 sites)
#   2. xp's E2004 keeps got "end of file" and expected "'}'"
#   3. E2005 keeps got = the offending token
#
# Negative controls (run 2026-09-28):
#   - TKC=<main 8f63fc6 build>: 1 fails at all three sites.
#   - put "fix" ahead of "got" in eerr_got: 3 fails (got is "").
#
# D109/D110 pin fix absence through the D-series runner; this script adds
# the "got" checks that runner cannot express.
#
# Story: 127.235
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c350_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C350: E2004/E2005 have no fix field and keep their got field (127.235)"
echo "------------------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# recs FILE CODE : "<n records>|<n with a fix key>|<first got>|<first expected>|<messages joined by ;>"
recs() { python3 -c '
import json,sys
recs=[]
for l in open(sys.argv[1]):
    l=l.strip()
    if not l.startswith("{"): continue
    d=json.loads(l)
    if d.get("error_code")==sys.argv[2]: recs.append(d)
if not recs: print("0|0|||"); sys.exit(0)
print("%d|%d|%s|%s|%s" % (len(recs), sum(1 for d in recs if "fix" in d), recs[0].get("got","<absent>"), recs[0].get("expected","<absent>"), ";".join(d.get("message","") for d in recs)))' "$1" "$2"; }

# E2004: xp() end-of-file branch, then eerr()'s "unclosed delimiter"
printf 'm=test;\nf=main():i64{<0' > e2004.tk
"${TKC}" --check --diag-json e2004.tk > e2004.log 2>&1; rc=$?
R="$(recs e2004.log E2004)"
expect "E2004: exit 1" "${rc}" "1"
expect "E2004: both sites emitted" "$(printf '%s' "${R}" | cut -d'|' -f5)" "expected '}', got end of file;unclosed delimiter"
expect "E2004: no record has a fix key" "$(printf '%s' "${R}" | cut -d'|' -f2)" "0"
expect "E2004 (xp): got/expected kept" "$(printf '%s' "${R}" | cut -d'|' -f3-4)" "end of file|'}'"

# E2005: eerr_got()
printf 'm=test;\nf=f(x:;):i64{<0};\n' > e2005.tk
"${TKC}" --check --diag-json e2005.tk > e2005.log 2>&1; rc=$?
R="$(recs e2005.log E2005)"
expect "E2005: exit 1" "${rc}" "1"
expect "E2005: no record has a fix key" "$(printf '%s' "${R}" | cut -d'|' -f2)" "0"
expect "E2005: got is the offending token" "$(printf '%s' "${R}" | cut -d'|' -f3)" ";"

echo ""
echo "C350: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
