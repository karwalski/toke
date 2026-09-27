#!/usr/bin/env bash
# C326_field_assignment_diagnostic.sh — `x.a=9;` says field assignment does not
# exist; it does not advise `==` (story 138.10, loke handoff T-26).
#
# THE DEFECT.  The grammar has no field store (AssignStmt = IDENT '=' Expr), so
# `x.a=9;` reached the expression parser, whose bare-`=` diagnostic advised
# "`=` is assignment; use `==` for equality".  Following that advice gave
# `x.a==9;`, which compiles to a comparison whose result is discarded: the
# program exited 1 where the author meant 9.
#
# DECIDED.  A bare `=` after `name.field` at the head of an expression statement
# is E2002 saying fields cannot be assigned and showing the rebuild.  No `fix`
# field: the rebuilt value needs the struct type and every other field.  The
# `==` advice stays where it is right (`if(x.a=1)`, `if(n=1)`).
#
# The second half of T-26 -- `x.a==9;` compiling without a warning -- is a
# discarded comparison (137.14) and belongs to the checker's discarded-value
# rule (src/types.c), not the parser; it is not asserted here.
#
# WHAT IS ASSERTED:
#   1. loke's T-26 repro: exit 1, one E2002 at the '=', message names field
#      assignment and does not advise '==', no fix field
#   2. `x.a.b=4;` (nested field) gets the same diagnostic
#   3. `if(x.a=1)` and `if(n=1)` keep the `==` advice (a condition, not a store)
#   4. the rebuild the message shows compiles and exits 9
#
# Negative control: TKC=<main c15dd11 build> fails 1 and 2.
#
# Story: 138.10
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c326_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C326: field assignment is diagnosed as such (138.10 / T-26)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# recs FILE -> "line:col:has_fix:message" per error record
recs() { python3 -c '
import json,sys
for l in open(sys.argv[1]):
    l=l.strip()
    if not l.startswith("{"): continue
    d=json.loads(l)
    if d.get("severity")!="error": continue
    print("%s|%d:%d|%s|%s" % (d["error_code"],d["pos"]["line"],d["pos"]["col"],"fix" if "fix" in d else "nofix",d["message"]))' "$1"; }

cat > t26.tk <<'TK'
m=s26;
t=$r{a:i64;b:i64};
f=main():i64{
  let x=mut.$r{a:1;b:2};
  x.a=9;
  <x.a
};
TK
"${TKC}" --check t26.tk > t26.log 2>&1; rc=$?
R="$(recs t26.log)"
expect "T-26: exit 1" "${rc}" "1"
expect "T-26: exactly one error record" "$(printf '%s\n' "${R}" | grep -c .)" "1"
expect "T-26: E2002 at the '=' (5:6), no fix field" "$(printf '%s' "${R}" | cut -d'|' -f1-3)" "E2002|5:6|nofix"
expect "T-26: the message says fields cannot be assigned" \
    "$(printf '%s' "${R}" | grep -c 'a struct field cannot be assigned')" "1"
expect "T-26: the message does not advise ==" "$(printf '%s' "${R}" | grep -c 'use `==`')" "0"

cat > nested.tk <<'TK'
m=s26;
t=$in{b:i64};
t=$r{a:$in};
f=main():i64{
  let x=mut.$r{a:$in{b:1}};
  x.a.b=4;
  <0
};
TK
"${TKC}" --check nested.tk > nested.log 2>&1
expect "x.a.b=4: the field-assignment E2002 at 6:8" \
    "$(recs nested.log | grep 'a struct field cannot be assigned' | cut -d'|' -f1-3)" "E2002|6:8|nofix"

cat > cond.tk <<'TK'
m=s26;
t=$r{a:i64;b:i64};
f=main():i64{
  let x=$r{a:1;b:2};
  let n=1;
  if(x.a=1){<3};
  if(n=1){<4};
  <0
};
TK
"${TKC}" --check cond.tk > cond.log 2>&1
expect "if(x.a=1) and if(n=1) keep the == advice" \
    "$(recs cond.log | grep -c 'use `==` for equality')" "2"
expect "neither condition is called a field store" \
    "$(recs cond.log | grep -c 'a struct field cannot be assigned')" "0"

cat > rebuild.tk <<'TK'
m=s26;
t=$r{a:i64;b:i64};
f=main():i64{
  let x=mut.$r{a:1;b:2};
  x=$r{a:9;b:x.b};
  <x.a
};
TK
if "${TKC}" --out rebuild_bin rebuild.tk > rebuild.log 2>&1; then
    ./rebuild_bin; expect "the rebuild the message shows exits 9" "$?" "9"
else expect "the rebuild the message shows compiles" "failed: $(head -c 200 rebuild.log)" "compiles"; fi

echo "------------------------------------------------------------"
echo "C326: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
