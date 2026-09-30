#!/usr/bin/env bash
# C561_single_sided_match_in_if_expr_tail.sh — a one-sided result match that
# is the VALUE of an expression-if (the last statement of one of its
# branches) is E4010, like any other used match (story 127.252 item 2,
# residue of 127.249 item 2 / C533).
#
# THE DEFECT.  types.c marked every match standing as a statement as
# discarded (127.195), and a branch block of `let a=if(c){ mt g() {$ok:v v} }
# el{5}` is a statement list -- so its tail match was taken for a discarded
# statement: no E4010, and a failure read as 0 (main printed 0).
#
# NOW: an `if` standing as a statement is discarded, and so are its
# branches' tails (an `el if` inherits that); any other `if` is an
# expression, and the last statement of each of its branch blocks is its
# value -- a match or nested if there is not discarded.  No `fix` (D200).
#
# Negative control: TKC=<main cde89021's toke> -- cases 1-5 fail (rc=0, no
# E4010).  Cases 6-9 are green on both.
#
# Story: 127.252 (2)
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
cd "${WORK}" || exit 1
HEAD='m=t;
i=io:std.io;
t=$ue{$bad:str};
f=g(n:i64):i64!$ue{ if(n<0){ <$bad("x") }; <n };'
# diags BODY: code|line|expected|fix-or-ABSENT of each E-diagnostic, then rc.
diags() { printf '%s\n%s\n' "${HEAD}" "$1" > p.tk
    "${TKC}" --check p.tk > o 2>&1; local rc=$?
    python3 - o "${rc}" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("error_code", "").startswith("E"):
        out.append("%s|%s|%s|%s" % (d["error_code"], d.get("pos", {}).get("line"),
                                    d.get("expected"), d.get("fix", "ABSENT")))
print(" / ".join(out + ["rc=" + sys.argv[2]]))
PY
}
ERR="an '\$err' arm"; OK="an '\$ok' arm"

expect "1 let = if, then-branch tail \$ok only" \
    "$(diags 'f=main():i64{ let c=true; let a=if(c){ mt g(0-1) {$ok:v v} }el{ 5 }; <a };')" \
    "E4010|5|${ERR}|ABSENT / rc=1"
expect "2 else-branch tail \$err only, after a statement" \
    "$(diags 'f=main():i64{ let c=false; let a=if(c){ 5 }el{ io.println("x");
  mt g(3) {$err:e 9} }; <a };')" \
    "E4010|6|${OK}|ABSENT / rc=1"
expect "3 an el-if chain in expression form" \
    "$(diags 'f=main():i64{ let n=2; let a=if(n==1){ 1 }el if(n==2){ mt g(0-1) {$ok:v v} }el{ 3 }; <a };')" \
    "E4010|5|${ERR}|ABSENT / rc=1"
expect "4 returned: <if(..){..}el{..}" \
    "$(diags 'f=h(c:bool):i64{ <if(c){ mt g(0-1) {$ok:v v} }el{ 0 } };
f=main():i64{ <h(true) };')" \
    "E4010|5|${ERR}|ABSENT / rc=1"
expect "5 nested: an if that is the tail of an expression-if is itself a value" \
    "$(diags 'f=main():i64{ let c=true; let d=true; let a=if(c){ if(d){ mt g(0-1) {$ok:v v} }el{ 1 } }el{ 2 }; <a };')" \
    "E4010|5|${ERR}|ABSENT / rc=1"
expect "6 control: a statement if (and its el-if) keeps one-sided tails" \
    "$(diags 'f=main():i64{ let n=2;
  if(n==1){ mt g(0-1) {$err:e io.println("e")} }el if(n==2){ mt g(2) {$ok:v io.println("o")} }el{ mt g(1) {$ok:v io.println("o")} };
  <0 };')" "rc=0"
expect "7 control: a one-sided match that is not the tail is a statement" \
    "$(diags 'f=main():i64{ let c=true; let a=if(c){ mt g(0-1) {$err:e io.println("e")}; 4 }el{ 5 }; <a };')" "rc=0"
expect "8 control: both sides in the tail" \
    "$(diags 'f=main():i64{ let c=true; let a=if(c){ mt g(0-1) {$ok:v v; $err:e 0} }el{ 5 }; <a };')" "rc=0"
expect "9 control: a statement if as a loop body tail" \
    "$(diags 'f=main():i64{ lp(let k=0;k<2;k=k+1){ if(k==0){ mt g(k) {$ok:v io.println("o")} } }; <0 };')" "rc=0"

echo "C561: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
