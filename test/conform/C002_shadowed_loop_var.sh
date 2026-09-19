#!/usr/bin/env bash
# C002_shadowed_loop_var.sh — runtime conformance for a loop variable that
# shadows an earlier binding of the same name (story 127.64).
#
# Codegen keeps a per-function registry of pointer locals, keyed by the RAW
# source name, that records which locals hold a string.  `==`, `!=`, `<`, `>`,
# `<=` and `>=` consult that registry and lower a string operand to strcmp.
#
# `let` drops a shadowed name's stale tag (126.7).  `lp(let x=…)` did not, so
#
#     let a = toks.get(i);          (* a is a $str -> tagged *)
#     lp(let a=0; a<n; a=a+1){ }    (* a is an i64 counter *)
#
# lowered the loop condition to strcmp(inttoptr i64 0, …) and the program died
# with SIGSEGV at the first iteration — clean --check, clean build, exit 139.
# That is exactly how L-GAZ-117 failed on tkc f99326281622.
#
# These cases compile and RUN, at every optimisation level, and assert the
# observed stdout.  A regression here is a crash, not a wrong string.
#
# Story: 127.64

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

WORK="$(mktemp -d /tmp/tkc_shadowloop_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C002: a loop variable shadowing a str binding is an integer, not a string"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE
#   Compiles SOURCE at -O0/-O1/-O2/-O3 and asserts stdout at each level.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "case.tk"
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f "case_bin"
        if ! "${TKC}" --allow-all "${o}" --out "case_bin" "case.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log
            FAIL=$((FAIL + 1))
            continue
        fi
        # A regression in this class can also spin forever (a bogus strcmp on
        # two garbage pointers never ends the loop), so cap each run.  macOS
        # ships no coreutils `timeout`; perl's alarm is always present.
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

# ── 1: the L-GAZ-117 shape — a str local, then a loop counter of the same
#      name.  `a<3` must be an integer compare, not strcmp on the constant 0. ──
run_case "str local then i64 loop var" $'x\n0\n1\n2' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("x y z");
  lp(let e=0;e<1;e=e+1){
    let a=toks.get(0);
    io.println(a);
  };
  lp(let a=0;a<3;a=a+1){ io.println(s.fromint(a)) };
  <0
};'

# ── 2: the counter is also USED as an index and in `==` — both must stay
#      integer operations. ──
run_case "shadowing counter indexes and compares" $'b\n1\nhit' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("a b c");
  let b=toks.get(1);
  io.println(b);
  let hit=mut."";
  lp(let b=0;b<3;b=b+1){
    if(b==1){ io.println(s.fromint(b)); hit="hit" };
  };
  io.println(hit);
  <0
};'

# ── 3: a str loop variable shadowing a str local STAYS a string — the clear
#      must not fire on a str->str shadow, or `==` would pointer-compare two
#      equal-content heap strings and report a miss. ──
run_case "str loop var over str local still strcmp" $'a\neq' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("a a");
  let w=toks.get(0);
  io.println(w);
  let n=mut.0;
  lp(let w=toks.get(1);n<1;n=n+1){
    if(w==toks.get(0)){ io.println("eq") }el{ io.println("ne") };
  };
  <0
};'

# ── 4: the reverse order — an i64 loop counter first, then a str local of the
#      same name.  The str binding must still compare by content. ──
run_case "i64 loop var then str local" $'0\n1\n2\nsame' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  lp(let v=0;v<3;v=v+1){ io.println(s.fromint(v)) };
  let toks=s.fields("same same");
  let v=toks.get(0);
  if(v==toks.get(1)){ io.println("same") }el{ io.println("diff") };
  <0
};'

# ── 5: nested loops re-using the name at both levels, with an outer str of
#      the same name — the L-GAZ-117 selection sort, reduced. ──
run_case "nested shadowing counters" $'p\n0:1\n0:2\n1:2' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("p q");
  let a=toks.get(0);
  io.println(a);
  lp(let a=0;a<3;a=a+1){
    lp(let b=a+1;b<3;b=b+1){
      io.println("\(a):\(b)");
    };
  };
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
