#!/usr/bin/env bash
# C533_single_sided_result_match_value.sh — a result match whose value is
# used must have both sides: `let a=mt g() {$ok:v v}` is E4010 at --check
# (story 127.249 item 2).
#
# THE DEFECT.  138.2 lowered the missing side of a one-armed result match as
# the zero value of the result type (it used to reach clang as an undefined
# label), and the checker accepted it: `let a=mt g(0-1) {$ok:v v}` made a
# failure read as 0, and `{$err:e 9}` a success read as 0 -- silently.
#
# NOW: E4010 (non-exhaustive match), no `fix`, when the match's value is used
# (a let, an argument, a return).  A match standing as a STATEMENT keeps one
# side (C400 case 7): an `$err` arm alone runs only on failure.
#
# Negative control: TKC=<main d3a083b8's toke> -- cases 1-4 fail (rc=0, no
# E4010).  Cases 5-7 are green on both.
#
# Story: 127.249
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
# diags BODY: code|expected|fix-or-ABSENT of each E-diagnostic, then rc.
diags() { printf '%s\n%s\n' "${HEAD}" "$1" > p.tk
    "${TKC}" --check p.tk > o 2>&1; local rc=$?
    python3 - o "${rc}" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("error_code", "").startswith("E"):
        out.append("%s|%s|%s" % (d["error_code"], d.get("expected"), d.get("fix", "ABSENT")))
print(" / ".join(out + ["rc=" + sys.argv[2]]))
PY
}

expect "1 let, \$ok only" "$(diags 'f=main():i64{ let a=mt g(0-1) {$ok:v v}; <a };')" \
    "E4010|an '\$err' arm|ABSENT / rc=1"
expect "2 let, \$err only" "$(diags 'f=main():i64{ let a=mt g(3) {$err:e 9}; <a };')" \
    "E4010|an '\$ok' arm|ABSENT / rc=1"
expect "3 returned, \$ok only" "$(diags 'f=h(n:i64):i64{ <mt g(n) {$ok:v v+1} };
f=main():i64{ <h(1) };')" "E4010|an '\$err' arm|ABSENT / rc=1"
expect "4 an argument, \$ok only" "$(diags 'f=main():i64{ io.println(mt g(1) {$ok:v "ok"}); <0 };')" \
    "E4010|an '\$err' arm|ABSENT / rc=1"
expect "5 control: statements keep one side" "$(diags 'f=main():i64{
  mt g(0-1) {$err:e io.println("only-err")};
  mt g(2) {$ok:v io.println("only-ok")};
  <0
};')" "rc=0"
expect "6 control: both sides, flat error variant" "$(diags 'f=main():i64{ let z=mt g(1) {$ok:v v; $bad:m 0}; <z };')" "rc=0"
expect "7 control: both sides, err first" "$(diags 'f=main():i64{ let z=mt g(1) {$err:e 0; $ok:v v}; <z };')" "rc=0"

echo "C533: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
