#!/usr/bin/env bash
# C214_lint_fn_value_let_has_no_fix.sh — lint's single-use-let hint carries
# no fix when the binding holds a function value (story 127.228).
#
# THE DEFECT, on main at c8575da: the fix inlined the initialiser into the
# one use whatever the binding held.  For a function value that is not the
# same program:
#   let sf=s.f; let n=sf(1)    ->  let n=s.f(1)     E4036 (a method call now)
#   let q=a.get(1); let r=q(3) ->  let r=a.get(1)(3)
#   let h=g; let z=h(1)        ->  let z=g(1)       (a deliberate copy)
# AGENTS §6: a fix is populated only when it is correct in every case.
#
# NOW (src/lint.c): the hint still fires, but the fix is absent when the use
# is the callee of a call, or when the initialiser names a function this file
# declares or a parameter declared with a function type.  Everything else
# keeps its fix (case 6).
#
# Negative control: TKC=<main's toke> -- cases 1-5 fail (a fix is present).
#
# Story: 127.228

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/p.tk" <<'EOF'
m=q;
t=$s{f:(i64):i64};
f=g(x:i64):i64{<x+1};
f=k(p:(i64):i64):i64{
  let pp=p;
  <pp(2)
};
f=main():i64{
  let s=$s{f:g};
  let sf=s.f;
  let n=sf(1);
  let a=@(g;g);
  let q=a.get(1);
  let r=q(3);
  let h=g;
  let z=h(1);
  let v=g;
  let u=k(v);
  let w=a;
  let y=w.len;
  <n+r+z+u+y
};
EOF

# The program itself is valid: the hints are about style, not errors.
"${TKC}" --check "${WORK}/p.tk" >/dev/null 2>&1
expect "0 program checks clean" "$?" "0"

LINT="$("${TKC}" --lint "${WORK}/p.tk" 2>&1)"
# fix_for NAME -> the replacement text of the single-use-let hint on NAME,
# "<none>" when the hint has no fix, "<no-hint>" when there is no hint.
fix_for() {
    printf '%s\n' "${LINT}" | awk -v n="$1" '
        index($0, "\"rule\":\"single-use-let\"") && index($0, "binding '\''" n "'\''") {
            if (match($0, /"replacement":"[^"]*"/)) print substr($0, RSTART+15, RLENGTH-16);
            else print "<none>";
            found=1; exit }
        END { if (!found) print "<no-hint>" }'
}

expect "1 s.f bound then called: no fix"          "$(fix_for sf)" "<none>"
expect "2 a.get(1) bound then called: no fix"     "$(fix_for q)"  "<none>"
expect "3 copy of a declared function: no fix"    "$(fix_for h)"  "<none>"
expect "4 function passed on as an argument: no fix" "$(fix_for v)" "<none>"
expect "5 function-typed parameter copied: no fix"   "$(fix_for pp)" "<none>"
expect "6 control: a plain value keeps its fix"   "$(fix_for w)"  "  let y=a"

echo "C214: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
