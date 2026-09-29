#!/usr/bin/env bash
# C400_mt_arms_bind_by_label.sh — a result match binds its arms by LABEL, in
# any order, and a user-declared error type is discriminated (story 138.2,
# loke T-18, T-31, T-51).
#
# THE DEFECT.  llvm.c's result lowering (emit_expr, NODE_MATCH_STMT) sent the
# first arm WRITTEN to the ok side of its one ok/err branch and every later arm
# to the err side, whatever their labels.  The checker was never at fault: it
# types the `$ok` binding by label (arm_bind_type).  Measured in emitted IR —
# `mt json.dec(s) {$jsonerr:e ..; $ok:o ..}` placed the $jsonerr body under
# `rm_ok`.  So a valid document ran the error arm and an invalid one ran the
# ok arm (T-31; T-51 is the same through json.dec), and a flat error-variant
# match written in any order but ok-first died with E9005.
#
# Separately, `<$err(x)` in a `T!$E` function (loke's T-18 spelling) lowered
# `$err(..)` as a pass-through of x and then took the return for an OK return:
# @tk_current_error was cleared, so the caller's match took `$ok` whatever it
# was written.  With both defects, T-18 exited 57 (first arm, twice).
#
# Negative control: TKC from main at 8efba9e — cases 1-7 red: 1 exits 57,
# 2 and 3 take the first arm written ("ok err err ok -1 0 -1", every
# statement match "stmt-err"/"stmt-jerr"), 4-6 fail to build with E9005 on
# their flat matches, 7 fails at clang on an undefined `%rm_err` label.
# Case 8, ok-first everywhere, is green on both: an ok-first match lowers
# exactly as it did.  Case 9 (two arms that name no side) builds on main and
# prints "inv:0"; it is E9005 now.
#
# Story: 138.2

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
cd "${WORK}" || exit 1
# run SRC [extra sources...] — build at -O0 and -O2; both must agree.
run() {
    local o out prev=""
    for o in -O0 -O2; do
        rm -f b
        "${TKC}" --allow-all "${o}" -I . --out b "$@" >/dev/null 2>&1 || { echo "build-failed${o}"; return; }
        out="$(./b 2>&1 | tr '\n' '|'; echo "rc=${PIPESTATUS[0]}")"
        if [ -n "${prev}" ] && [ "${prev}" != "${out}" ]; then echo "O-levels differ: ${prev} / ${out}"; return; fi
        prev="${out}"
    done
    echo "${prev}"
}

# 1. loke T-18 verbatim: `mt` on X!$userErr, both orders (exit 77).
cat > t18.tk <<'EOF'
m=g;
t=$e{ $bad:str };
f=check():i64!$e{ <$err($bad("no")) };
f=main():i64{
  let a=mt check() { $ok:v 5; $err:e 7 };
  let b=mt check() { $err:e 7; $ok:v 5 };
  <a*10+b
};
EOF
expect "1-T18-user-error-type" "$(run t18.tk)" "rc=77"

# 2. loke T-31 / T-51: a stdlib result, ok-first/err-first x valid/invalid,
#    over json.dec (a pointer sentinel) and str.toint (the error channel).
cat > s1.tk <<'EOF'
m=s1;
i=io:std.io;
i=json:std.json;
i=s:std.str;
f=main():i64{
  let a=mt json.dec("{\"a\":1}") { $ok:o "ok"; $jsonerr:e "err" };
  let b=mt json.dec("{\"a\":1}") { $jsonerr:e "err"; $ok:o "ok" };
  let c=mt json.dec("{{{nope") { $ok:o "ok"; $jsonerr:e "err" };
  let d=mt json.dec("{{{nope") { $jsonerr:e "err"; $ok:o "ok" };
  let e=mt s.toint("42") { $err:x 0-1; $ok:n n };
  let g=mt s.toint("zz") { $err:x 0-1; $ok:n n };
  let h=mt s.toint("0") { $err:x 0-1; $ok:n n };
  io.println("\(a) \(b) \(c) \(d) \(e) \(g) \(h)");
  <0
};
EOF
expect "2-stdlib-result-both-orders" "$(run s1.tk)" "ok ok err err 42 -1 0|rc=0"

# 3. Nested matches, and match as a statement, err arm first.
cat > n1.tk <<'EOF'
m=n1;
i=io:std.io;
i=json:std.json;
t=$ue{$bad:str};
f=chk(n:i64):i64!$ue{ if(n<0){ <$err($bad("x")) }; <n };
f=main():i64{
  let a=mt json.dec("{\"method\":\"hi\"}") { $jsonerr:e "dec-err"; $ok:o mt json.str(o;"method") { $jsonerr:e "str-err"; $ok:s "str-ok:\(s)" } };
  let b=mt json.dec("{not json") { $jsonerr:e "dec-err"; $ok:o mt json.str(o;"method") { $jsonerr:e "str-err"; $ok:s "str-ok:\(s)" } };
  let c=mt chk(1) { $err:e 0-9; $ok:v mt chk(0-v) { $err:e 100; $ok:w w } };
  mt chk(0-5) { $err:e io.println("stmt-err"); $ok:v io.println("stmt-ok") };
  mt chk(5) { $err:e io.println("stmt-err"); $ok:v io.println("stmt-ok") };
  mt json.dec("[1]") { $jsonerr:e io.println("stmt-jerr"); $ok:o io.println("stmt-jok") };
  io.println("\(a) \(b) \(c)");
  <0
};
EOF
expect "3-nested-and-statement" "$(run n1.tk)" "stmt-err|stmt-ok|stmt-jok|str-ok:hi dec-err 100|rc=0"

# 4. A user T!$userErr across a module boundary (loke's case), every spelling
#    of the error return, every arm order, flat variant arms included.
cat > lib.tk <<'EOF'
m=lib;
t=$usererr{$bad:str;$worse:i64};
f=check(n:i64):i64!$usererr{
  if(n<0){ <$err($bad("neg")) };
  if(n>100){ <$worse(n) };
  <n*2
};
EOF
"${TKC}" --emit-interface --check -I . lib.tk >/dev/null 2>&1
cat > x1.tk <<'EOF'
m=main;
i=io:std.io;
i=lib:lib;
f=main():i64{
  let a=mt lib.check(0-1) { $ok:v 5; $err:e 7 };
  let b=mt lib.check(0-1) { $err:e 7; $ok:v 5 };
  let c=mt lib.check(3) { $err:e 0-1; $ok:v v };
  let d=mt lib.check(3) { $ok:v v; $err:e 0-1 };
  let e=mt lib.check(500) { $worse:w w; $ok:v v; $bad:m 0-2 };
  let g=mt lib.check(0-1) { $bad:m m.len as i64; $worse:w w; $ok:v v };
  let h=mt lib.check(9) { $bad:m 0-3; $ok:v v; $worse:w w };
  io.println("\(a) \(b) \(c) \(d) \(e) \(g) \(h)");
  <0
};
EOF
expect "4-user-error-cross-module" "$(run x1.tk lib.tk)" "7 7 6 6 500 3 18|rc=0"

# 5. A 3-variant user sum matched in a non-declaration order, and a 3-variant
#    error sum matched flat with `$ok` third.
cat > u3.tk <<'EOF'
m=u3;
i=io:std.io;
t=$shape{$circle:i64;$square:i64;$tri:i64};
t=$e3{$p:i64;$q:str;$r:i64};
f=name(s:$shape):str{ <mt s { $tri:t "tri\(t)"; $circle:c "circle\(c)"; $square:q "square\(q)" } };
f=g(n:i64):i64!$e3{
  if(n==1){ <$p(10) };
  if(n==2){ <$q("qq") };
  if(n==3){ <$r(30) };
  <n
};
f=h(n:i64):str{ <mt g(n) { $r:x "r\(x)"; $q:s "q\(s)"; $ok:v "ok\(v)"; $p:x "p\(x)" } };
f=main():i64{
  io.println("\(name($shape{$circle:1})) \(name($shape{$square:2})) \(name($shape{$tri:3}))");
  io.println("\(h(1)) \(h(2)) \(h(3)) \(h(4))");
  <0
};
EOF
expect "5-three-variant-sums" "$(run u3.tk)" "circle1 square2 tri3|p10 qqq r30 ok4|rc=0"

# 6. The error channel through a let binding, void!$E, an ok value of 0, and
#    the err payload read by a nested variant match.
cat > w1.tk <<'EOF'
m=w1;
i=io:std.io;
t=$ue{$bad:str;$code:i64};
f=gv(n:i64):void!$ue{ if(n<0){ <$err($code(n)) }; io.println("gv-ran") };
f=gz(n:i64):i64!$ue{ if(n<0){ <$err($bad("z")) }; <0 };
f=main():i64{
  let c=mt gv(0-3) { $err:e mt e { $bad:m 0; $code:k k }; $ok:v 1 };
  let d=mt gv(3) { $err:e 99; $ok:v 1 };
  let r=gz(0-1);
  let x=mt r { $err:e "err"; $ok:v "ok\(v)" };
  let r2=gz(1);
  let y=mt r2 { $err:e "err"; $ok:v "ok\(v)" };
  let z=mt gz(1) { $bad:m "bad"; $ok:v "ok\(v)"; $code:k "code" };
  io.println("\(c) \(d) \(x) \(y) \(z)");
  <0
};
EOF
expect "6-let-bound-void-and-zero" "$(run w1.tk)" "gv-ran|-3 1 err ok0 ok0|rc=0"

# 7. A single-sided match: an `$err` arm alone runs only on failure, an `$ok`
#    arm alone only on success.
cat > o2.tk <<'EOF'
m=o2;
i=io:std.io;
t=$ue{$bad:str};
f=g(n:i64):i64!$ue{ if(n<0){ <$bad("x") }; <n };
f=main():i64{
  mt g(0-1) { $err:e io.println("only-err") };
  mt g(1) { $err:e io.println("only-err-WRONG") };
  mt g(0-1) { $ok:v io.println("only-ok-WRONG") };
  mt g(2) { $ok:v io.println("only-ok \(v)") };
  <0
};
EOF
expect "7-single-sided" "$(run o2.tk)" "only-err|only-ok 2|rc=0"

# 8. Control: ok-first matches (the form nearly every program uses).
cat > k1.tk <<'EOF'
m=k1;
i=io:std.io;
i=json:std.json;
t=$ue{$bad:str};
f=g(n:i64):i64!$ue{ if(n<0){ <$bad("x") }; <n };
f=main():i64{
  let a=mt json.dec("{\"a\":1}") { $ok:o "ok"; $jsonerr:e "err" };
  let b=mt json.dec("{{") { $ok:o "ok"; $jsonerr:e "err" };
  let c=mt g(4) { $ok:v v; $err:e 0-1 };
  let d=mt g(0-4) { $ok:v v; $err:e 0-1 };
  io.println("\(a) \(b) \(c) \(d)");
  <0
};
EOF
expect "8-control-ok-first" "$(run k1.tk)" "ok err 4 -1|rc=0"

# 9. Two arms, neither `$ok`/`$err` nor a variant of an established sum:
#    there is no dispatch to lower, so the build stops with E9005 (as it does
#    at 3+ arms) instead of choosing on value-vs-zero.  Main at 8efba9e built
#    this and printed "inv:0" (Batch E1, C411 case 5).
cat > a1.tk <<'EOF'
m=a1;
i=io:std.io;
t=$ue{$notfound:str;$invalid:str};
f=h(x:i64):str{ let e=x; <mt e {$notfound:m "nf:\(m)";$invalid:m "inv:\(m)"} };
f=main():i64{ io.println(h(0)); <0 };
EOF
expect "9-two-unlabelled-arms-E9005" \
    "$("${TKC}" --out b a1.tk 2>&1 | grep -o '"error_code":"E9005"' | head -1)" '"error_code":"E9005"'

echo "C400: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
