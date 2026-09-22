#!/usr/bin/env bash
# C023_cross_scope_shadowing.sh — a `let` that shadows an enclosing binding
# compiles, means what the spec says it means, and is warned about (137.6).
#
# THREE separable things, which the original report conflated into one:
#
#  1. THE IR.  The backend writes LLVM IR as text in source order, so the
#     `alloca` for a `let` inside an if/lp body was emitted inside that
#     branch's basic block while its uses — and, after the unique-local
#     renaming, the uses of the OUTER binding of the same name — sat in
#     blocks the branch does not dominate.  clang rejected the whole module
#     with "Instruction does not dominate all uses!", whether or not the
#     inner value escaped the branch.  Every occurrence was a build break.
#
#  2. THE MEANING.  Hoisting the allocas alone is not enough, and shipping
#     only that would have been worse than the bug: the alias table that maps
#     a toke name to its LLVM slot was never popped at the end of a block, so
#     a read of the outer `x` AFTER the block still resolved to the inner
#     `%x.1`.  The module would then be valid and the answer wrong — silently.
#     Case 2 below is the one that catches that, and it asserts a VALUE.
#
#  3. THE WARNING.  Story 75.1.7 chose "Option A. Allow same-scope and
#     cross-scope shadowing" and the spec publishes `let x = x + 1;` as legal
#     (toke-spec-v0.3.md §11.8.1), so this must NOT be rejected.  75.1.7 also
#     reserved the other half — "optional lint rule: mixed-mut-shadow" —
#     because legal is not the same as silent.  W3013 is that rule: the 137
#     migration found 52 shadowed bindings across 18 files, including a
#     privacy opt-out never honoured and four job-queue counters that never
#     advanced, and every one of them compiled.
#
# Case 7 is the prerequisite defect: W5001 fired on the read of the OUTER
# binding, at function-body level, claiming it was "bound in a nested block
# (depth 1)" — because the bind side-table was never popped and its backwards
# search found the inner entry.  A second shadowing warning standing beside a
# misattributed first one would have compounded the confusion.
#
# Stories: 137.6, 75.1.7, 126.7, 114.7, 110.7

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

WORK="$(mktemp -d /tmp/tkc_shadow_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C023: cross-scope let shadowing — valid IR, correct value, one warning"
echo "----------------------------------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE — builds and runs at every -O level.
#
# Asserted against a RUN, not a compile, at all four optimisation levels:
# the defect this replaces was an invalid-IR build break, and the half that
# replaced it would have been a wrong answer that compiled.  Only a value
# separates those two.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "ok.tk"
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f ok_bin
        if ! "${TKC}" --allow-all "${o}" --out "ok_bin" "ok.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log | head -4
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

# warns CODE NAME LINE SOURCE — `--check` must emit CODE, on that line.
# The line is asserted, not just the code: a diagnostic that names the wrong
# line is what sends the automated repair loop to the wrong place (127.116).
warns() {
    local code="$1" name="$2" line="$3" src="$4"
    printf '%s\n' "${src}" > "w.tk"
    local out
    out="$("${TKC}" --check "w.tk" 2>&1)"
    if printf '%s' "${out}" | grep -q "\"error_code\":\"${code}\"" &&
       printf '%s' "${out}" | grep "\"error_code\":\"${code}\"" | grep -q "\"line\":${line},"; then
        echo "  PASS: ${name} — ${code} at line ${line}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected ${code} at line ${line}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -4
        FAIL=$((FAIL + 1))
    fi
}

# silent CODE NAME SOURCE — `--check` must NOT emit CODE.
silent() {
    local code="$1" name="$2" src="$3"
    printf '%s\n' "${src}" > "s.tk"
    local out
    out="$("${TKC}" --check "s.tk" 2>&1)"
    if printf '%s' "${out}" | grep -q "\"error_code\":\"${code}\""; then
        echo "  FAIL: ${name}: ${code} fired and must not have"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: ${name} — no ${code}"
        PASS=$((PASS + 1))
    fi
}

# ── Case 1: the reproducer, exactly as reported ──────────────────────────
# Before: clang refused the module — "Instruction does not dominate all
# uses!  %x.1 = alloca i64 / %t13 = load i64, ptr %x.1" — at every -O level.
run_case "if-body shadow builds and runs" "inner 2
outer 1" \
'm=main;
i=io:std.io;
f=main():i64{
  let x=1;
  if(x>0){
    let x=2;
    io.println("inner \(x)");
  };
  io.println("outer \(x)");
  <0
};'

# ── Case 2: the outer binding is UNCHANGED after the block ───────────────
# The case that separates "the IR is valid now" from "the program is right
# now".  With the allocas hoisted but the name aliases still leaking out of
# the block, this built cleanly and printed 2 — the shadowing `let` would
# have looked like an assignment after all, which is the very confusion the
# warning exists to flag.  Asserted as a return code as well as stdout so a
# print-path quirk cannot mask it.
run_case "the outer binding survives the block" "1
1" \
'm=main;
i=io:std.io;
f=g():i64{
  let x=1;
  if(x>0){
    let x=99;
  };
  <x
};
f=main():i64{
  let x=1;
  if(x>0){ let x=99; };
  io.println("\(x)");
  io.println("\(g())");
  <0
};'

# ── Case 3: a loop body is the same rule ─────────────────────────────────
# loke's four infinite job-queue transitions were this shape: a `let i=i+1`
# inside the body declares a second i and the outer counter never moves.
# It must compile, it must keep the outer value, and it must warn.
run_case "loop-body shadow keeps the outer value" "0
1
2
7" \
'm=main;
i=io:std.io;
f=g():i64{
  let n=7;
  lp(let j=0;j<3;j=j+1){
    let n=j;
    io.println("\(n)");
  };
  <n
};
f=main():i64{io.println("\(g())");<0};'

# ── Case 4: the shadowed binding may have a different type (126.7) ───────
# 126.7 fixed a same-scope re-bind to a new type; the cross-scope form was
# never reachable because it did not build.  Pinned so the two agree.
run_case "cross-scope shadow with a new type" "ab
5" \
'm=main;
i=io:std.io;
f=main():i64{
  let v=5;
  if(v>0){
    let v="ab";
    io.println("\(v)");
  };
  io.println("\(v)");
  <0
};'

# ── Case 5: three levels deep, and an el branch ──────────────────────────
run_case "nested blocks and an el branch" "3
2
1" \
'm=main;
i=io:std.io;
f=main():i64{
  let x=1;
  if(x>0){
    let x=2;
    if(x>0){
      let x=3;
      io.println("\(x)");
    }el{
      let x=4;
      io.println("\(x)");
    };
    io.println("\(x)");
  };
  io.println("\(x)");
  <0
};'

# ── Case 6: the warning ──────────────────────────────────────────────────
# What 75.1.7 reserved and 137.6 decided to ship.  The `let` line, not the
# enclosing block and not the read.
warns "W3013" "if-body shadow warns" 6 \
'm=main;
i=io:std.io;
f=main():i64{
  let x=1;
  if(x>0){
    let x=2;
    io.println("\(x)");
  };
  <0
};'
warns "W3013" "loop-body shadow warns" 6 \
'm=main;
i=io:std.io;
f=g():i64{
  let n=7;
  lp(let j=0;j<3;j=j+1){
    let n=j;
    io.println("\(n)");
  };
  <n
};
f=main():i64{<g()};'
warns "W3013" "a loop variable shadowing an outer binding warns" 5 \
'm=main;
i=io:std.io;
f=main():i64{
  let i=1;
  lp(let i=0;i<3;i=i+1){
    io.println("\(i)");
  };
  <0
};'
warns "W3013" "shadowing a parameter warns" 4 \
'm=main;
i=io:std.io;
f=g(x:i64):i64{
  let x=x+1;
  <x
};
f=main():i64{io.println("\(g(1))");<0};'

# ── The regression guard: shadowing stays LEGAL and mostly silent ────────
# The spec's own published example (§11.8.1).  A fix that rejected — or even
# warned on — this would contradict a decision this project already took.
silent "W3013" "the spec's same-scope example does not warn" \
'm=main;
i=io:std.io;
f=main():i64{
  let x=1;
  let x=x+1;
  io.println("\(x)");
  <0
};'
silent "E3012" "the spec's same-scope example is not an error" \
'm=main;
i=io:std.io;
f=main():i64{
  let x=1;
  let x=x+1;
  io.println("\(x)");
  <0
};'
run_case "the spec's same-scope example still yields 2" "2" \
'm=main;
i=io:std.io;
f=main():i64{
  let x=1;
  let x=x+1;
  io.println("\(x)");
  <0
};'
silent "W3013" "a fresh name in a nested block does not warn" \
'm=main;
i=io:std.io;
f=main():i64{
  let x=1;
  if(x>0){ let y=2; io.println("\(y)"); };
  <0
};'
silent "W3013" "two sibling blocks each binding the same fresh name" \
'm=main;
i=io:std.io;
f=main():i64{
  let c=1;
  if(c>0){ let y=2; io.println("\(y)"); };
  if(c>0){ let y=3; io.println("\(y)"); };
  <0
};'

# ── Case 7: the prerequisite — W5001 was pointing at the wrong binding ───
# `<x` returns the FUNCTION-BODY binding.  Nothing escapes.  W5001 fired on
# it anyway, claiming depth 1, because the bind side-table kept the entry the
# if-body left behind and lookup_bind_depth searches backwards.
# Annotated `:i64` on purpose: W5001 skips a TY_UNKNOWN value (110.7), and an
# un-annotated `let` is often exactly that, so the un-annotated spelling would
# pass this case for the wrong reason and prove nothing.
silent "W5001" "no escape warning on a function-body binding that was shadowed" \
'm=main;
f=g():i64{
  let x:i64=1;
  if(x>0){ let x:i64=2; };
  <x
};
f=main():i64{<g()};'
silent "W5001" "no escape warning on a loop-shadowed function-body binding" \
'm=main;
f=g():i64{
  let n:i64=7;
  lp(let j=0;j<3;j=j+1){ let n:i64=j; };
  <n
};
f=main():i64{<g()};'

# W5001 must still fire where it means something — 110.7's positive
# regression: a primitive bound INSIDE the block and returned from inside it.
warns "W5001" "a genuine nested-block escape still warns" 5 \
'm=main;
f=g(c:i64):i64{
  if(c>0){
    let r:i64=42;
    <r
  };
  <0
};
f=main():i64{<g(1)};'

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
