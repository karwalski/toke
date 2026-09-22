#!/usr/bin/env bash
# C027_unknown_subject_match.sh — a `mt` of 3+ arms over a value whose sum
# type the compiler has not established is a diagnostic, not a backend crash
# (story 137.2).
#
# llvm.c has three match lowerings, tried in order:
#   1. the sum-tag switch — needs expr_struct_type() to resolve the scrutinee
#      to a registered `is_sum` type AND the first arm to name one of its
#      variants;
#   2. the 3+-arm string chain — needs an i8* scrutinee;
#   3. the result-match (`$ok`/`$err`) bifurcation.
#
# (3) is structurally BINARY.  It dispatches once on `br i1` over
# @tk_current_error and then labels arm 0 `rm_okN` and every later arm
# `rm_errN` — so a third arm re-opens a block that is already terminated and
# clang rejects the module: "Terminator found in the middle of a basic block!".
#
# THE LOKE REPORT'S SUGGESTED FIX IS THE WRONG MECHANISM.  It asks for the
# label counter to be per-arm.  Unique labels would produce unreachable blocks
# and a dispatch that still only ever chose between two of them, because there
# is exactly one condition and no tag to switch on.  The report's own
# SECONDARY suggestion is the real fix, and is what this pins: reaching (3)
# with 3+ arms means neither (1) nor (2) could establish what is being matched
# on, so the compiler is proceeding on something it never established.  Say so.
#
# The report's eight-row table of which shapes link is unreliable and is not
# reproduced here: several rows that "link" return the wrong answer via
# 127.113 (a tail expression without `<` silently returns 0), because every
# T-2 program in the report is written in that shape.  Every program below
# ends its success path in an explicit `<`.
#
# Cases 1-2: the defect — the two shapes that reproduce (a sum value read back
#            out of an array, and one passed through an i64-typed parameter).
#            Both must now be refused with E9005 and must NOT produce a binary.
# Case 3:    a 3-arm match on a value WITH a known sum type still compiles and
#            still dispatches to all three arms.  This is the guard that
#            matters: a fix that refused every 3-arm match would pass 1-2 and
#            delete the language feature.
# Case 4:    2-arm `$ok`/`$err` matches — the shape the binary lowering is
#            actually for — are untouched, on both arms.
# Case 5:    a 2-arm match over an unknown subject is still allowed.  The
#            diagnostic is about arity exceeding what the lowering can
#            dispatch, not about unknown subjects as such; widening it to all
#            unknown subjects would break case 4's stdlib shapes.
#
# Stories: 137.2, 127.113, 127.56, 114.41
#
# The E9005 message names the arm count and, when the sum type DID resolve but
# the first arm names no variant of it, names both — so the two ways of
# reaching the same lowering are distinguishable in the diagnostic.

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

WORK="$(mktemp -d /tmp/tkc_c027_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C027: a 3+-arm match on an unestablished subject is a diagnostic"
echo "----------------------------------------------------------------"

# reject_build NAME SOURCE — must be refused with E9005, and must not leave a
# binary behind.  Asserted against --out, not --check: the guarantee is that
# no such program reaches a runnable binary.
reject_build() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "case.tk"
    local out rc=0
    rm -f case_bin
    out="$("${TKC}" --allow-all --out case_bin "case.tk" 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && [ ! -x case_bin ] && printf '%s' "${out}" | grep -q 'E9005'; then
        echo "  PASS: ${name} refused with E9005"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected the compiler to refuse with E9005, got rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -3
        [ -x case_bin ] && echo "      it produced a binary, which printed: $(./case_bin 2>&1)"
        FAIL=$((FAIL + 1))
    fi
}

# run_case NAME EXPECTED_STDOUT SOURCE — asserts stdout at -O0/-O1/-O2/-O3.
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

SUM='m=main;
i=io:std.io;
t=$shape{$circle:i64;$square:i64;$tri:i64};
f=mk(k:i64):$shape{
  if(k==0){
    <$circle(1)
  };
  if(k==1){
    <$square(2)
  };
  <$tri(3)
};'

# ── Case 1: a sum value read back out of an array ───────────────────────
# `xs.get(0)` gives an i64; nothing records that it is a $shape, so the tag
# switch declines and the 3 arms fall into the binary lowering.
reject_build "3-arm match on an array element" \
"${SUM}
f=main():i64{
  let xs=@(mk(0));
  let a=xs.get(0);
  let r=mt(a){\$circle:x 10;\$square:x 20;\$tri:x 30};
  io.println(\"\\(r)\");
  <0
};"

# ── Case 2: a sum value passed through an i64-typed parameter ───────────
reject_build "3-arm match on an i64-typed parameter" \
"${SUM}
f=pick(sh:i64):i64{
  <mt(sh){\$circle:x 10;\$square:x 20;\$tri:x 30}
};
f=main():i64{
  io.println(\"\\(pick(mk(0) as i64))\");
  <0
};"

# ── Case 3: the guard — a KNOWN sum type still dispatches all three arms ─
# Asserted on all three variants, not just one: a lowering that fell back to
# the binary form would still get the first arm right.
run_case "3-arm match on a known sum type" "10 20 30" \
"${SUM}
f=area(sh:\$shape):i64{
  <mt(sh){\$circle:r 10;\$square:r 20;\$tri:r 30}
};
f=main():i64{
  io.println(\"\\(area(mk(0))) \\(area(mk(1))) \\(area(mk(2)))\");
  <0
};"
run_case "3-arm match on a local of known sum type" "10 20 30" \
"${SUM}
f=main():i64{
  let a=mk(0);
  let b=mk(1);
  let c=mk(2);
  let ra=mt(a){\$circle:x 10;\$square:x 20;\$tri:x 30};
  let rb=mt(b){\$circle:x 10;\$square:x 20;\$tri:x 30};
  let rc=mt(c){\$circle:x 10;\$square:x 20;\$tri:x 30};
  io.println(\"\\(ra) \\(rb) \\(rc)\");
  <0
};"

# ── Case 4: 2-arm $ok/$err — what the binary lowering is actually for ────
# Both arms asserted.  This is the shape every error union in the language
# uses; if the diagnostic reached it, nothing would compile.
run_case "2-arm \$ok/\$err on both arms" "7 -1" \
'm=main;
i=io:std.io;
t=$myerr{$bad:$str};
f=num(x:i64):i64!$myerr{
  if(x<0){
    <$bad("negative")
  };
  <$ok(x+2)
};
f=one(x:i64):i64{
  <mt(num(x)){$ok:v v;$err:e 0-1}
};
f=main():i64{
  io.println("\(one(5)) \(one(0-1))");
  <0
};'

# ── Case 5: a 2-arm match on an unknown subject is still allowed ─────────
# The rule is about arity exceeding what the lowering can dispatch, not about
# unknown subjects as such.  Widening it to every unknown subject would
# reject the stdlib error-union shapes in case 4, whose scrutinee type is
# likewise not a registered sum.
run_case "2-arm match on an unestablished subject still builds" "10" \
"${SUM}
f=main():i64{
  let xs=@(mk(0));
  let a=xs.get(0);
  let r=mt(a){\$circle:x 10;\$square:x 20};
  io.println(\"\\(r)\");
  <0
};"

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
