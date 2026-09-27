#!/usr/bin/env bash
# C370_string_match_every_arity.sh — a match over a plain string compares the
# scrutinee against every arm tag, at every arity and every arm position
# (127.237 (1), Batch GB2).
#
# THE DEFECT.  The strcmp-chain lowering (80.2.7) was gated on "scrutinee is
# i8* AND 3+ arms".  Anything else fell through to the RESULT lowering, which
# dispatches on value-vs-null, so:
#   * 2 arms: `mt s {$x:w 1;$y:w 2}` with s="y" gave 1 — every non-null
#     string took arm 0.  A silent wrong arm (the row's 3-arm example is
#     correct on main; the wrong arm is the 2-arm form).
#   * 1 arm: a branch to a label never defined — clang error, E9003.
#   * a string that arrives as an i64 word — a call, `mt g(s){…}`,
#     `mt str.trim(s){…}` — missed the chain and died with E9005 at 3+ arms
#     ("'$x' is not a variant of sum type '$str'").
# Now a plain `str` scrutinee whose arms are not the result tags
# ($ok/$err) is a string match at any arity.
#
# The non-matching control pins the chain's documented default: a string
# that names no arm takes the LAST arm (80.2.7 "Last arm: unconditional").
#
# Negative control: TKC built from main at 9f901bf — cases 2, 3, 5, 6 red
# ("1 1 1", build-failed, build-failed, build-failed); 1, 4, 7 green.
#
# Story: 127.237 (1)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
run() { printf '%s\n' "$1" > "${WORK}/p.tk"; rm -f "${WORK}/b"
        "${TKC}" "${WORK}/p.tk" -o "${WORK}/b" >/dev/null 2>&1 || { echo build-failed; return; }
        "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

# 1. 3 and 4 arms: every position, plus a non-matching string (last arm).
expect "1-three-four-arms" "$(run 'm=main;i=io:std.io;
f=p3(s:$str):i64{<mt s {$y:w 1;$z:w 2;$x:w 3}};
f=p4(s:$str):i64{
  let r=mt s {$a:w 10;$b:w 20;$c:w 30;$d:w 40};
  <r
};
f=main():i64{
  io.println("\(p3("y")) \(p3("z")) \(p3("x")) \(p3("none"))");
  io.println("\(p4("a")) \(p4("b")) \(p4("c")) \(p4("d")) \(p4("none"))");
  <0
};')" "1 2 3 3 10 20 30 40 40 "

# 2. 2 arms: both positions, and the non-matching control.
expect "2-two-arms" "$(run 'm=main;i=io:std.io;
f=p2(s:$str):i64{<mt s {$x:w 1;$y:w 2}};
f=main():i64{ io.println("\(p2("x")) \(p2("y")) \(p2("none"))"); <0 };')" "1 2 2 "

# 3. 1 arm: builds, and every string takes it.
expect "3-one-arm" "$(run 'm=main;i=io:std.io;
f=p1(s:$str):i64{<mt s {$x:w 7}};
f=main():i64{ io.println("\(p1("x")) \(p1("none"))"); <0 };')" "7 7 "

# 4. The binding is the scrutinee string in every arm (let-bound, 3 arms).
expect "4-arm-binding" "$(run 'm=main;i=io:std.io;
f=main():i64{
  let s="zz";
  let r=mt s {$x:w "X";$y:w "Y";$zz:w w};
  io.println("r=\(r)");
  <0
};')" "r=zz "

# 5. A user call as scrutinee, 3 arms, every position.
expect "5-user-call-scrutinee" "$(run 'm=main;i=io:std.io;
f=g(a:$str):$str{<a};
f=p(s:$str):i64{<mt g(s) {$x:w 1;$y:w 2;$z:w 3}};
f=main():i64{ io.println("\(p("x")) \(p("y")) \(p("z"))"); <0 };')" "1 2 3 "

# 6. A std call as scrutinee, 2 and 3 arms, binding read in the arm.
expect "6-std-call-scrutinee" "$(run 'm=main;i=io:std.io;i=str:std.str;
f=p2(s:$str):i64{<mt str.lower(s) {$x:w 1;$y:w w.len+1}};
f=p3(s:$str):$str{<mt str.trim(s) {$x:w "X";$y:w "Y";$z:w w}};
f=main():i64{ io.println("\(p2("X")) \(p2("Y")) \(p3(" x ")) \(p3(" y")) \(p3("zz "))"); <0 };')" "1 2 X Y zz "

# 7. Control: a 2-arm RESULT match over a str error union still dispatches
#    on the error channel, not on the tag text.
expect "7-result-match-unchanged" "$(run 'm=main;i=io:std.io;
t=$e{$bad:i64};
f=h(k:i64):$str!$e{ if(k>0){<"pos"}; <$e{$bad:k} };
f=main():i64{
  let a=mt h(1) {$ok:v v;$err:e "err"};
  let b=mt h(0) {$ok:v v;$err:e "err"};
  io.println("\(a) \(b)");
  <0
};')" "pos err "

echo "C370: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
