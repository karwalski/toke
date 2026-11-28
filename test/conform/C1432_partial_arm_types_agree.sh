#!/usr/bin/env bash
# C1432_partial_arm_types_agree.sh -- an expression-if / mt binding whose
# arms carry the SAME partial type takes that type (story 127.375).
#
# `let d=if(c){@("k":s.bytes("ab"))}el{@("k":s.bytes("cd"))}` -- each tail is
# `@(str:?)` (s.bytes' [byte] is not typed).  if_expr_bind_type (127.326) and
# mt_arms_agreed_type (127.327) required tk_type_identical between arms, which
# answers 0 whenever either side holds TY_UNKNOWN, so the binding was left
# untyped and `d.get("k")` lowered as an ARRAY read: RT003.  A single such arm
# was already adopted (127.374, C1420 case 4).
#
# The arms now agree by arm_types_agree: tk_type_identical, except that an
# unknown below the top level matches an unknown in the same position.  The
# adopted type is no more than either arm's own type.  Not a join rule: a
# position unknown in one arm and known in the other still disagrees, and so
# does a different shape (case 9 stays untyped; 10-11 are E4011, as before).
#
# Negative control: TKC built from integrate/z-wave-1113b (c0a8bee7) --
# cases 1-3 trap RT003, 4-8 rc=0 (binding untyped); 9-12 green on both.
#
# Story: 127.375, 127.559

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
H='m=pa;i=s:std.str;i=io:std.io;t=$er{m:str};f=r(n:i64):i64!$er{<n};f=g(x:str):i64{<s.len(x) as i64};'
chk() {
    printf '%s\nf=main():i64{\nlet c=true;\n%s\n<0};\n' "$H" "$1" > "${WORK}/cb.tk"
    local out rc g
    out="$("${TKC}" --check "${WORK}/cb.tk" 2>&1)"; rc=$?
    g="$(printf '%s' "$out" | grep -o '"error_code":"[A-Z0-9]*"\|"got":"[^"]*"' | head -2 | cut -d'"' -f4 | tr '\n' ' ')"
    echo "rc=${rc} ${g:-none}"
}
run() {
    printf '%s\nf=main():i64{\nlet c=true;\n%s\n<0};\n' "$H" "$1" > "${WORK}/rb.tk"
    tk_build "${TKC}" "${WORK}/rb.tk" -o "${WORK}/rb" >/dev/null 2>&1 || { tk_build_failed $?; return; }
    tk_run "${WORK}/rb" 2>&1 | tr '\n' ' '
}
IF='let d=if(c){@("k":s.bytes("ab"))}el{@("k":s.bytes("cd"))};'
IFF='let d=if(false){@("k":s.bytes("ab"))}el{@("k":s.bytes("cd"))};'
MT='let d=mt r(1) {$ok:v @("k":s.bytes("ab")); $err:e @("k":s.bytes("cd"))};'
GET='let e=d.get("k"); io.println("\(e.get(1))");'
expect "1 if, then-branch: d.get(k) reads the [byte]" "$(run "${IF} ${GET}")" "98 "
expect "2 if, else-branch" "$(run "${IFF} ${GET}")" "100 "
expect "3 mt arms" "$(run "${MT} ${GET}")" "98 "
expect "4 if binding into an i64 let: E4031" "$(chk "${IF} let y:i64=d;")" "rc=1 E4031 @(str:?) "
expect "5 mt binding into a str parameter: E4031" "$(chk "${MT} io.println(\"\\(g(d))\");")" "rc=1 E4031 @(str:?) "
expect "6 el-if chain of three" \
  "$(chk 'let d=if(c){@(s.bytes("a"))}el if(false){@(s.bytes("b"))}el{@(s.bytes("c"))}; let y:i64=d;')" "rc=1 E4031 @(?) "
expect "7 let mut binding" "$(chk 'let d=mut.if(c){@(s.bytes("a"))}el{@(s.bytes("b"))}; let y:i64=d;')" "rc=1 E4031 @(?) "
expect "8 nested: map of arrays of the partial" \
  "$(chk 'let d=if(c){@("k":@(s.bytes("a")))}el{@("k":@(s.bytes("b")))}; let y:i64=d;')" "rc=1 E4031 @(str:@(?)) "
expect "9 control: unknown in one arm, known in the other -- untyped (no join)" \
  "$(chk 'let d=if(c){@(s.bytes("ab"))}el{@(@(1;2))}; let y:i64=d;')" "rc=0 none"
expect "10 control: different shape -- E4011 at the tails, as before" \
  "$(chk 'let d=if(c){@("k":s.bytes("ab"))}el{@(s.bytes("cd"))};')" "rc=1 E4011 @(?) "
expect "11 control: different shape in mt arms -- E4011, as before" \
  "$(chk 'let d=mt r(1) {$ok:v @("k":s.bytes("ab")); $err:e @(1:s.bytes("cd"))};')" "rc=1 E4011 @(i64:?) "
expect "12 control: fully typed arms as before" \
  "$(chk 'let d=if(c){@("k":@(1;2))}el{@("k":@(3))}; let y:i64=d;')" "rc=1 E4031 @(str:@(i64)) "

echo "C1432: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
