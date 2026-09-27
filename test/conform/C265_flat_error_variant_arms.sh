#!/usr/bin/env bash
# C265_flat_error_variant_arms.sh — a match over a `T!$e` value whose arms
# after `$ok` name the variants of the error sum type directly builds, and
# each variant arm binds that variant's payload (story 127.231 (1)).
#
# THE DEFECT. `mt g(5) {$ok:v v;$a:p 0;$b:q 0}` passed `--check` and died
# at build with E9005 ("the sum type of the matched value is not
# established"): the result-match lowering is binary (ok/err on the error
# channel) and only the sum-tag switch handles 3+ arms, which does not apply
# to a `T!$e` scrutinee.  At exactly two arms (`{$ok:v v;$a:p p}`) it built
# and bound `p` to the error BOX ADDRESS rather than the payload — a silent
# wrong value (printed r=4379335448 for a payload of 41).  llvm.c now
# recognises the flat spelling (every arm after `$ok` a variant of the error
# sum) and switches on the payload box's tag.
#
# Negative control: TKC built from main at d236e6f — case 1 "build-failed",
# case 2 "r=<address>", cases 3-4 "build-failed"; case 5 green on both (the
# `$err:e` spelling is unchanged).
#
# Story: 127.231 (1)

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
runs() {
    "${TKC}" --check "$1" >/dev/null 2>&1 || { echo "check-failed"; return; }
    "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" 2>/dev/null | tr '\n' ' '; echo "rc=${PIPESTATUS[0]}"
}

# 1. the row's repro: three arms, direct call scrutinee
cat > "${WORK}/p1.tk" <<'EOF'
m=t;
t=$e{$a:i64;$b:bool};
f=g(x:i64):i64!$e{
  if(x==0){<$e{$a:1}};
  <x
};
f=main():i64{
  <mt g(5) {$ok:v v;$a:p 0;$b:q 0}
};
EOF
expect "1-three-arms-builds" "$(runs "${WORK}/p1.tk")" "rc=5"

# 2. two arms: the variant arm binds the PAYLOAD, not the box
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
t=$e{$a:i64};
f=g(x:i64):i64!$e{
  if(x==0){<$e{$a:41}};
  <x
};
f=main():i64{
  let r=mt g(0) {$ok:v v;$a:p p};
  io.println("r=\(r)");
  <0
};
EOF
expect "2-two-arms-payload" "$(runs "${WORK}/p2.tk")" "r=41 rc=0"

# 3. every variant payload type (i64, bool, str, f64), arms in any order,
#    direct and let-bound scrutinee, ok path
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
t=$e{$a:i64;$b:bool;$c:str;$d:f64};
f=g(x:i64):i64!$e{
  if(x==0){<$e{$a:41}};
  if(x==1){<$e{$b:true}};
  if(x==2){<$e{$c:"cee"}};
  if(x==3){<$e{$d:2.5}};
  <x
};
f=h(x:i64):i64{
  <mt g(x) {$ok:v v*10;$a:p p+100;$b:q 200;$c:s s.len+300;$d:d 400}
};
f=k(x:i64):str{
  let r=g(x);
  <mt r {$ok:v "ok";$c:s s;$a:p "a";$b:q "b";$d:d "d"}
};
f=main():i64{
  lp(let i=0;i<5;i=i+1){ io.println("\(h(i)) \(k(i))") };
  <0
};
EOF
expect "3-all-variants" "$(runs "${WORK}/p3.tk")" "141 a 200 b 303 cee 400 d 40 ok rc=0"

# 4. f64 payload is read as a double
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;
t=$e{$n:i64;$d:f64};
f=g(x:i64):i64!$e{ if(x==3){<$e{$d:2.5}}; <x };
f=main():i64{
  let r=mt g(3) {$ok:v 0.0;$n:p 1.0;$d:d d*3.0};
  io.println("\(r)");
  <0
};
EOF
expect "4-f64-payload" "$(runs "${WORK}/p4.tk")" "7.5 rc=0"

# 5. control: the `$err:e` spelling is unchanged
cat > "${WORK}/p5.tk" <<'EOF'
m=t;
t=$e{$a:i64;$b:bool};
f=g(x:i64):i64!$e{
  if(x==0){<$e{$a:1}};
  <x
};
f=main():i64{
  <mt g(0) {$ok:v v;$err:e 7}
};
EOF
expect "5-err-arm-unchanged" "$(runs "${WORK}/p5.tk")" "rc=7"

echo "C265: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
