#!/usr/bin/env bash
# C004_loop_step_after_return.sh — a loop whose body ends in an early return
# does not emit its step expression after the terminator (story 127.70).
#
#     lp(let w=toks.get(0); cond; w=toks.get(1)){ io.println(w); <0 };
#
# failed E9003 on a clean `--check` — on the pinned build and on the
# pre-regression build alike, so this is a standing defect, not a regression.
#
# Mechanism.  emit_stmt's NODE_LOOP case emits the body, then the step, then
# the back-edge.  The back-edge was guarded by `c->term` (the flag that says
# the body already emitted a terminator); the step was not.  A body ending in
# `<expr` emits `ret`, and the step's instructions were written straight after
# it.  LLVM's textual parser tolerates dead instructions after a terminator, so
# a trivial `i=i+1` step slipped through — but a step that opens its own basic
# blocks does not: `w = toks.get(i)` emits the RT003 bounds-check labels, and
# the parser fails at the next label with "expected instruction opcode" /
# "Terminator found in the middle of a basic block".  That is why the defect
# surfaces on a loop variable rebound to a `$str`.
#
# A terminated body reaches the header through its own terminator (or leaves
# the function), so its step is dead code either way.
#
# Story: 127.70

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_loopstep_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C004: a loop step is not emitted after the body's own terminator"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE
#   Compiles at -O0..-O3 and asserts stdout.  A regression is a hard E9003 at
#   compile time, so the compile step is itself the assertion.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > case.tk
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f case_bin
        if ! "${TKC}" --allow-all "${o}" --out case_bin case.tk >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log | head -6
            FAIL=$((FAIL + 1))
            continue
        fi
        out="$(perl -e 'alarm 20; exec @ARGV' ./case_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${o}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected [${expected}], got [${out}] (rc ${rc})"
            FAIL=$((FAIL + 1))
        fi
    done
}

# ── 1: the reported shape — a str loop variable, a bounds-checked step, and a
#      body that returns on the first iteration. ──
run_case "str loop var, bounds-checked step, early return" $'a' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("a b c");
  lp(let w=toks.get(0);s.contains(w;"a");w=toks.get(1)){
    io.println(w);
    <0
  };
  <0
};'

# ── 2: the same with an integer counter — valid IR before the fix only
#      because the dead instructions opened no basic block. ──
run_case "i64 loop var, early return" $'x' '
m=t;
i=io:std.io;
f=main():i64{
  lp(let i=0;i<3;i=i+1){
    io.println("x");
    <0
  };
  <0
};'

# ── 3: the loop variable rebound to a str literal in the step. ──
run_case "str loop var, literal step, early return" $'a' '
m=t;
i=io:std.io;
f=main():i64{
  lp(let w="a";w!="z";w="z"){
    io.println(w);
    <7
  };
  <0
};'

# ── 4: a CONDITIONAL return must still run the step on the iterations that
#      do not return — the guard against skipping a live step. ──
run_case "conditional return still advances the loop" $'a\nb\nc' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("a b c");
  let n=mut.0;
  lp(let w=toks.get(0);n<9;w=toks.get(n)){
    io.println(w);
    n=n+1;
    if(n>2){<0};
  };
  <0
};'

# ── 5: a body ending in `br` (break) is terminated too; the loop must still
#      leave through loop_exit with the right value. ──
run_case "break as the last body statement" $'a' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("a b c");
  let n=mut.0;
  lp(let w=toks.get(0);n<9;w=toks.get(1)){
    io.println(w);
    n=n+1;
    br
  };
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
