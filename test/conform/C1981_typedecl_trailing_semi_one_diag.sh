#!/usr/bin/env bash
# C1981_typedecl_trailing_semi_one_diag.sh — a trailing ';' before '}' in a
# type declaration is ONE E2002, not a four-error cascade (story 127.339).
#
# HISTORY.  parse_field_list's comment said a trailing ';' before '}' was
# allowed, but the loop broke WITHOUT consuming it, so the caller's '}' check
# saw ';' and `m=a;t=$p{x:i64;};` gave E2002, E2004, E2002, E2002.
# grammar.ebnf (FieldList = Field { ';' Field }) admits no trailing ';', so it
# stays refused: one E2002 at the ';' (no fix), the ';' is skipped, and the
# rest of the file parses.  Trailing ';' in lists generally is 127.360 (owner).
#
# WHAT IS ASSERTED (each: exactly the listed diagnostics, by --check)
#   1. `t=$p{x:i64;};`                         -> E2002 once, at col 15, got ';'
#   2. two fields, multi-line, trailing ';'    -> E2002 once
#   3. sum type `t=$q{$ok:i64;$no:str;};`      -> E2002 once
#   4. two offending decls                     -> E2002 twice (one each)
#   5. the rest of the file parses on (no cascade into main): E2002 once
#      (a later type error is not reported: checking stops after a parse error)
#   6. no trailing ';' -> clean (exit 0)
#
# Negative control: TKC=<v3.2.0 tkc> fails 1-5 (four diagnostics for case 1).
#
# Story: 127.339
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1981_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
# codes <file>: the error codes --check reports, space-separated, then the exit code
codes() {
    local out rc
    out="$("${TKC}" --check "$1" 2>&1)"; rc=$?
    printf '%s\n' "${out}" | sed -n 's/.*"error_code":"\([A-Z][0-9]*\)".*/\1/p' | tr '\n' ' ' | sed 's/ $//'
    printf ' rc=%s' "${rc}"
}
printf 'm=a;t=$p{x:i64;};\n' > "${WORK}/1.tk"
printf 'm=a;\nt=$p{\n  x:i64;\n  y:str;\n};\nf=main():i64{<0};\n' > "${WORK}/2.tk"
printf 'm=a;t=$q{$ok:i64;$no:str;};f=main():i64{<0};\n' > "${WORK}/3.tk"
printf 'm=a;t=$p{x:i64;};t=$r{y:str;};f=main():i64{<0};\n' > "${WORK}/4.tk"
printf 'm=a;t=$p{x:i64;};f=main():i64{let s:str=1;<0};\n' > "${WORK}/5.tk"
printf 'm=a;t=$p{x:i64;y:str};f=main():i64{<0};\n' > "${WORK}/6.tk"
expect "1-one-e2002"     "$(codes "${WORK}/1.tk")" "E2002 rc=1"
col="$("${TKC}" --check "${WORK}/1.tk" 2>&1 | sed -n 's/.*"col":\([0-9]*\).*"got":"\([^"]*\)".*/\1 \2/p')"
expect "1-at-semicolon"  "${col}" "15 ;"
expect "2-multiline"     "$(codes "${WORK}/2.tk")" "E2002 rc=1"
expect "3-sum-type"      "$(codes "${WORK}/3.tk")" "E2002 rc=1"
expect "4-two-decls"     "$(codes "${WORK}/4.tk")" "E2002 E2002 rc=1"
expect "5-recovers-no-cascade" "$(codes "${WORK}/5.tk")" "E2002 rc=1"
expect "6-clean"         "$(codes "${WORK}/6.tk")" " rc=0"
echo "C1981: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
