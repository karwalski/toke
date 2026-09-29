#!/usr/bin/env bash
# C372_arm_shadow_registry_restored.sh — a match arm whose binding shadows
# an outer name re-tags that raw name in the pointer registry for the arm
# only; after the arm the outer binding's tag is back (127.237 (3), Batch
# GB2).
#
# THE DEFECT.  127.71 records a shadowing arm binding's tag ("$str", a
# struct name) under the RAW source name, because every use inside the arm
# looks the raw name up — by blanking the outer binding's entries and
# appending its own.  Nothing undid it when the arm ended, so after
#   let v=2.5; let r=mt s {$a:v 1;$b:v v.len;$c:v 3};
# the outer `v` was still "$str": `v<3.0` lowered to tk_str_cmp on a double
# (clang: "defined with type 'double' but expected 'ptr'", E9003).  The
# same holds for every lowering that re-tags: string chain, sum-tag switch,
# result match ($ok arm), flat error-variant arms.  Each arm now saves the
# raw name's entries before the re-tag and restores them after its body.
#
# Negative control: TKC built from main at 9f901bf — cases 1-4 red
# (build-failed, E9003).  Injection (ptr_shadow_restore made a no-op) ->
# 1-4 red again.
#
# Story: 127.237 (3)

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

# 1. String chain: the arm binds a str under an outer f64's name.
expect "1-string-chain" "$(run 'm=main;i=io:std.io;
f=main():i64{
  let v=2.5;
  let s="b";
  let r=mt s {$a:v 1;$b:v v.len;$c:v 3};
  let lt=v<3.0;
  io.println("r=\(r) lt=\(lt) v=\(v)");
  <0
};')" "r=1 lt=true v=2.5 "

# 2. Sum-tag switch: a $str payload arm under an outer f64's name.  The str
#    arm is the LAST one — each arm re-tags, so on main the leak is whatever
#    the last shadowing arm left (an i64 arm leaves the tag blank).
expect "2-sum-switch" "$(run 'm=main;i=io:std.io;
t=$sh{$nm:$str;$n:i64};
f=pick(x:$sh):i64{
  let v=2.5;
  let r=mt x {$n:v v;$nm:v v.len};
  let lt=v<3.0;
  if(lt){<r+100};
  <r
};
f=main():i64{ io.println("k=\(pick($sh{$nm:"abc"}))"); <0 };')" "k=103 "

# 3. Result match: the $ok arm binds a str result under an outer f64's name.
expect "3-result-ok-arm" "$(run 'm=main;i=io:std.io;
t=$e{$bad:i64};
f=h(k:i64):$str!$e{ if(k>0){<"pos"}; <$e{$bad:k} };
f=main():i64{
  let v=2.5;
  let r=mt h(1) {$ok:v v.len;$err:e 0};
  let lt=v<3.0;
  io.println("r=\(r) lt=\(lt)");
  <0
};')" "r=3 lt=true "

# 4. Flat error-variant arm: a $str payload under an outer f64's name.
expect "4-flat-variant-arm" "$(run 'm=main;i=io:std.io;
t=$e{$bad:$str;$gone:i64};
f=h(k:i64):i64!$e{ if(k>0){<k}; <$e{$bad:"no"} };
f=main():i64{
  let v=2.5;
  let r=mt h(0) {$ok:x x;$bad:v v.len;$gone:q q};
  let lt=v<3.0;
  io.println("r=\(r) lt=\(lt)");
  <0
};')" "r=2 lt=true "

# 5. Control: inside the arm the shadowing binding still has its own tag.
expect "5-inside-arm" "$(run 'm=main;i=io:std.io;
f=main():i64{
  let w=7;
  let s="hello";
  let r=mt s {$a:w 1;$b:w 2;$c:w w.len};
  io.println("r=\(r) w=\(w+1)");
  <0
};')" "r=5 w=8 "

echo "C372: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
