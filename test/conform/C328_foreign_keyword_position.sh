#!/usr/bin/env bash
# C328_foreign_keyword_position.sh — W1020 "foreign keyword detected" fires
# where the foreign keyword is meant, not on a toke name spelled the same
# (story 138.10, loke handoff N-4).
#
# THE DEFECT.  The lexer warned on every identifier spelled like a keyword of
# another language, wherever it stood: `let def:$shorthanddef=...` drew
# "Python keyword 'def' detected; toke uses f=name():type{body}" at the binding
# and at every use -- 43 warnings in one loke module.
#
# DECIDED.  Any such word is a toke name at a binding site: after `let`, or
# before `=` (not `==`), or before a `:` that does not end the line.  A word
# that is a statement/declaration keyword elsewhere (def, return, class, var,
# else, while, ...) is also a name when an operator or closing punctuation
# follows it (`def.a`, `f(def)`, `<def;`).  Value-like words (None, null, nil,
# undefined, True, False) and the output calls (print, printf, console) are
# meant in exactly those value positions, so only binding sites exempt them.
#
# WHAT IS ASSERTED (W1020 count per line, and the program's exit):
#   1. loke's shape -- a binding named def and its uses -- no W1020 at all,
#      and the program runs (exit 3)
#   2. a parameter, a struct field and an import alias spelled like foreign
#      keywords -- no W1020
#   3. the foreign keywords as meant still warn, one per line: `def f():`,
#      `return x`, `else {`, `while (x)`, `class Foo:`, `var x = 1`, `else:`
#   4. value-like words in value position still warn: `x=null;`,
#      `print("x")`, `console.log(..)`, `<True`
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c328_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C328: W1020 fires only where the foreign keyword is meant (138.10 / N-4)"
echo "------------------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# w1020 FILE -> the lines carrying a W1020, space separated
w1020() { "${TKC}" --check "$1" 2>&1 | python3 -c '
import json,sys
ls=[]
for l in sys.stdin:
    l=l.strip()
    if l.startswith("{"):
        d=json.loads(l)
        if d.get("error_code")=="W1020": ls.append(str(d["pos"]["line"]))
print(" ".join(ls) if ls else "none")'; }

cat > n4.tk <<'TK'
m=n4;
t=$shorthanddef{a:i64};
f=main():i64{
  let def:$shorthanddef=$shorthanddef{a:3};
  let n=def.a;
  <def.a
};
TK
expect "binding named def and its uses: no W1020" "$(w1020 n4.tk)" "none"
if "${TKC}" --out n4_bin n4.tk > n4.log 2>&1; then
    ./n4_bin; expect "the def program runs: exit 3" "$?" "3"
else expect "the def program compiles" "failed: $(head -c 200 n4.log)" "compiles"; fi

cat > names.tk <<'TK'
m=names;
i=use:std.str;
t=$cls{class:i64;var:i64};
f=pick(def:i64;null:i64):i64{<def+1};
f=main():i64{
  let c=$cls{class:1;var:2};
  let r=pick(c.class;c.var);
  let else=mut.0;
  else=r;
  <else
};
TK
expect "parameter, field, alias and assigned names: no W1020" "$(w1020 names.tk)" "none"

cat > meant.tk <<'TK'
m=meant;
def f():
return x
else {
while (x)
class Foo:
var x = 1
else:
TK
expect "foreign keywords as meant: one W1020 on each line 2-8" "$(w1020 meant.tk)" "2 3 4 5 6 7 8"

cat > values.tk <<'TK'
m=values;
f=main():i64{
  x=null;
  print("x");
  console.log("y");
  <True
};
TK
expect "value-like words in value position still warn (lines 3-6)" "$(w1020 values.tk)" "3 4 5 6"

echo "------------------------------------------------------------------------"
echo "C328: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
