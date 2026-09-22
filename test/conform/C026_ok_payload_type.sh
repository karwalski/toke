#!/usr/bin/env bash
# C026_ok_payload_type.sh — `$ok(x)` has the type of `x` (story 137.1).
#
# `$ok(e)` is not a call.  emit_expr lowers a one-argument call to a name that
# is neither a function nor a known sum variant by PASSING THE ARGUMENT
# THROUGH unchanged (llvm.c, "Sum type variant constructor ... the variant
# value IS the payload"), so the value that comes out has the argument's LLVM
# type.  expr_llvm_type's matching tail answered "i64" for every such call.
#
# Every site that coerces on the strength of that answer then emitted a
# conversion whose operand was a different width.  In a `bool!$myerr`
# function the declared return type is i1 (the error travels out-of-band in
# @tk_current_error), so the return path took the i64->i1 arm and emitted
#
#     %t13 = trunc i64 %t12 to i1
#
# on a %t12 that was already i1 — "'%t12' defined with type 'i1' but expected
# 'i64'".  An f64 payload produced the same trunc on a double.
#
# THE LOKE REPORT'S STATED MECHANISM IS WRONG.  It reports `inttoptr i64` on
# an i1 and asks for a `zext i1 ... to i64` before it.  There is no inttoptr
# on this path and a zext would not have helped: the coercion site is correct
# and complete — it was being told the wrong source type.  This is the same
# class 127.27 fixed for map-literal values and 127.93 fixed for
# sub-namespace calls; both times the rule was "the two answers must not
# diverge", and both times it was applied to one site rather than to the
# oracle.  Here it is applied to the oracle.
#
# Cases 1-2: the defect — a bool and an f64 `$ok` payload, both arms, values
#            asserted (not merely "it links").
# Case 3:    the three workarounds the report says do not work: a bool
#            literal, a bool bound to a local first, and a branch returning
#            $ok(true)/$ok(false).
# Case 4:    i64 and str payloads, which already worked, must keep working.
# Case 5:    an ordinary one-argument user function call must not be caught by
#            the new rule — the pass-through fires only when the name is NOT a
#            function, and a fix that widened it would retype every call.
# Case 6:    a real variant constructor still builds its tagged box (127.56).
#
# Every success path ends in an explicit `<` so that 127.113 (a tail
# expression without `<` silently returning 0) cannot be mistaken for a
# result of this fix.
#
# Stories: 137.1, 127.27, 127.93, 127.56

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c026_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C026: \$ok(x) has the type of x"
echo "----------------------------------------------------------------"

run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "ok.tk"
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f ok_bin
        if ! "${TKC}" --allow-all "${o}" --out "ok_bin" "ok.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log | head -3
            FAIL=$((FAIL + 1))
            continue
        fi
        out="$(./ok_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${o}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected '${expected}', got '${out}' (rc ${rc})"
            FAIL=$((FAIL + 1))
        fi
    done
}

# ── Case 1: the defect — a bool payload, both arms, values asserted ─────
# `check(20)` is $ok(true), `check(5)` is $ok(false), `check(-1)` is $err.
# Printing 1/0/-1 pins that the bool actually survives the return, which
# "it compiles" alone would not.
run_case "bool payload, both values and the err arm" "1 0 -1" \
'm=main;
i=io:std.io;
i=s:std.str;
t=$myerr{$bad:$str};
f=check(x:i64):bool!$myerr{
  if(x<0){
    <$bad("negative")
  };
  <$ok(x>10)
};
f=b2i(b:bool):i64{
  if(b){
    <1
  };
  <0
};
f=one(x:i64):i64{
  <mt(check(x)){$ok:v b2i(v);$err:e 0-1}
};
f=main():i64{
  io.println("\(one(20)) \(one(5)) \(one(0-1))");
  <0
};'

# ── Case 2: an f64 payload — the same defect, a different width ─────────
# Before the fix this emitted `trunc i64 %t to i1`... on a double, at the
# f64 return arm.  Included because a fix special-cased to i1 would pass
# case 1 and leave this one broken.
run_case "f64 payload" "2.5" \
'm=main;
i=io:std.io;
t=$myerr{$bad:$str};
f=half(x:f64):f64!$myerr{
  if(x<0.0){
    <$bad("negative")
  };
  <$ok(x/2.0)
};
f=main():i64{
  let r=mt(half(5.0)){$ok:v v;$err:e 0.0};
  io.println("\(r)");
  <0
};'

# ── Case 3: the three workarounds the report says also fail ─────────────
# The report states that binding the bool to a local first, using a bool
# literal, and branching so each arm returns $ok(true)/$ok(false) all fail
# identically.  That is correct, and it follows from the mechanism: all
# three are still `$ok(<an i1>)`.  Pinned so a fix that only handled the
# comparison-expression spelling is caught.
run_case "bool literal payload" "1" \
'm=main;
i=io:std.io;
t=$myerr{$bad:$str};
f=yes():bool!$myerr{
  <$ok(true)
};
f=b2i(b:bool):i64{
  if(b){
    <1
  };
  <0
};
f=main():i64{
  let r=mt(yes()){$ok:v b2i(v);$err:e 0-1};
  io.println("\(r)");
  <0
};'
run_case "bool bound to a local first" "0" \
'm=main;
i=io:std.io;
t=$myerr{$bad:$str};
f=no():bool!$myerr{
  let b=1>2;
  <$ok(b)
};
f=b2i(b:bool):i64{
  if(b){
    <1
  };
  <0
};
f=main():i64{
  let r=mt(no()){$ok:v b2i(v);$err:e 0-1};
  io.println("\(r)");
  <0
};'
run_case "branch returning \$ok(true)/\$ok(false)" "1 0" \
'm=main;
i=io:std.io;
t=$myerr{$bad:$str};
f=pick(x:i64):bool!$myerr{
  if(x>0){
    <$ok(true)
  };
  <$ok(false)
};
f=b2i(b:bool):i64{
  if(b){
    <1
  };
  <0
};
f=one(x:i64):i64{
  <mt(pick(x)){$ok:v b2i(v);$err:e 0-1}
};
f=main():i64{
  io.println("\(one(1)) \(one(0))");
  <0
};'

# ── Case 4: the payload widths that already worked must keep working ────
run_case "i64 and str payloads still work" "7 hi" \
'm=main;
i=io:std.io;
t=$myerr{$bad:$str};
f=num(x:i64):i64!$myerr{
  if(x<0){
    <$bad("negative")
  };
  <$ok(x+2)
};
f=word():$str!$myerr{
  <$ok("hi")
};
f=main():i64{
  let a=mt(num(5)){$ok:v v;$err:e 0};
  let b=mt(word()){$ok:v v;$err:e "no"};
  io.println("\(a) \(b)");
  <0
};'

# ── Case 5: a real one-argument user function must not be retyped ───────
# The new rule fires only when lookup_fn fails.  If it ever fired on a real
# call, `dbl(x)` would take the ARGUMENT's type instead of the declared
# return type — here an f64 argument through an i64-returning function.
run_case "a real 1-arg user call keeps its declared return type" "3" \
'm=main;
i=io:std.io;
f=trunc1(x:f64):i64{
  <x as i64
};
f=main():i64{
  io.println("\(trunc1(3.7))");
  <0
};'

# ── Case 6: a genuine variant constructor still boxes (127.56) ──────────
# variant_ctor_sum() must still win over the pass-through, or the tagged
# [tag,payload] box that 127.56 added is lost and `mt` mis-dispatches.
run_case "variant constructor still boxes and dispatches" "20" \
'm=main;
i=io:std.io;
t=$shape{$circle:i64;$square:i64};
f=mk(k:i64):$shape{
  if(k==0){
    <$circle(1)
  };
  <$square(2)
};
f=main():i64{
  let a=mk(1);
  let r=mt(a){$circle:x 10;$square:x 20};
  io.println("\(r)");
  <0
};'

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
