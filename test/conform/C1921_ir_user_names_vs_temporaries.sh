#!/usr/bin/env bash
# C1921_ir_user_names_vs_temporaries.sh — a user name can never be the name
# of a value or block codegen mints itself (story 138.38, loke T-65).
#
# THE DEFECT.  `f=g(t0:i64):i64{<t0+1}` passed --check and clang refused the
# IR: "multiple definition of local value named 't0'" (E9003).  A parameter's
# slot was %<name>, and %t0 is also the first temporary; `t5` met the fifth
# the same way.  114.15 had aliased LOCALS named t<N>, but neither parameters
# nor closure captures went through that rule, and nothing covered the other
# minted names: LLVM keeps block labels and values in one per-function
# namespace, so a local or parameter named like a label (svarm3, svchk5, ...)
# collided too ("'%svarm3' is not a basic block").
#
# NOW.  Every minted name is a prefix and a counter (or starts with `_`), so a
# user name that ends in a digit is always aliased (%t0.N), for parameters,
# closure parameters, closure captures and locals alike.  Other names keep
# their spelling in the IR.
#
# WHAT THIS DRIVES.  Each program builds and runs at -O0 and -O2 and prints
# its hand-derived line; case `plain` asserts a name without a trailing digit
# keeps its spelling.
#
# Negative control: v3.2.0 (ea7ddf22) — params, labels and loop fail to
# build ("multiple definition of local value named 't0'", "'%svarm3' is not
# a basic block"); `control` builds there too, and so does `closure`, only
# because a lifted body's temporaries happen to be numbered past t0/t3 there.
#
# Story: 138.38

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
[ -x "${TKC}" ] || { echo "FAIL: tkc binary not found at ${TKC}"; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1921_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1921: user names never collide with codegen's temporaries and labels"

# run NAME WANT SOURCE
run() {
    local name="$1" want="$2" o
    printf '%s\n' "$3" > "${WORK}/${name}.tk"
    if ! "${TKC}" --check "${WORK}/${name}.tk" >"${WORK}/${name}.chk" 2>&1; then
        fail "${name}: --check rejected it: $(head -c 200 "${WORK}/${name}.chk")"; return
    fi
    for o in -O0 -O2; do
        if ! "${TKC}" --allow-all "${o}" --out "${WORK}/${name}" "${WORK}/${name}.tk" >"${WORK}/${name}.b" 2>&1; then
            fail "${name} ${o}: did not build: $(grep -m1 -E 'error' "${WORK}/${name}.b" | cut -c1-200)"; continue
        fi
        local got; got="$("${WORK}/${name}" </dev/null 2>&1 | tr '\n' '|')"
        [ "${got}" = "${want}" ] && pass "${name} ${o}: ${want}" \
            || fail "${name} ${o}: got [${got}] want [${want}]"
    done
}

# 1. loke's T-65: a parameter t0 (the first temporary), and t5 (the fifth).
run params '5|21|' 'm=c1921a;
i=io:std.io;
f=g(t0:i64):i64{<t0+1};
f=h(t5:i64;t1:str):i64{
  let a=t5*2;
  let b=a+t5;
  let c=b-1;
  <c+t5+t1.len
};
f=main():i64{
  io.println("\(g(4))");
  io.println("\(h(5;"ab"))");
  <0
};'
# 2. block-label names: a parameter svchk5 and a local svarm3 in a function
#    whose sum match mints those labels.  2*3+1 = 7, then 3*3+4 = 13.
run labels '7|13|' 'm=c1921b;
i=io:std.io;
t=$sh{$circle:i64;$sq:i64};
f=area(s:$sh;svchk5:i64):i64{
  let svarm3=1;
  <mt s {$circle:r r*3+svarm3;$sq:w w*w+svchk5}
};
f=main():i64{
  io.println("\(area($sh{$circle:2};4))");
  io.println("\(area($sh{$sq:3};4))");
  <0
};'
# 3. a closure parameter t0 and a closure capture t3.  6*2 = 12, 1+10 = 11.
run closure '12|11|' 'm=c1921c;
i=io:std.io;
f=main():i64{
  let k=fn(t0:i64):i64{<t0*2};
  let t3=10;
  let q=fn(x:i64):i64{<x+t3};
  io.println("\(k(6))");
  io.println("\(q(1))");
  <0
};'
# 4. a parameter t2 read in a loop bound, and copied into a mut local t0.
#    0+1+2+3 = 6; 1+3 = 4.
run loop '6|4|' 'm=c1921d;
i=io:std.io;
f=sum(t2:i64):i64{
  let s=mut.0;
  lp(let i=0;i<t2;i=i+1){ s=s+i };
  <s
};
f=bump(t1:i64):i64{
  let t0=mut.t1;
  t0=t0+3;
  <t0
};
f=main():i64{
  io.println("\(sum(4))");
  io.println("\(bump(1))");
  <0
};'
# 5. the T-65 control (a local t0) still builds: exit value 5.
run control '5|' 'm=c1921e;
i=io:std.io;
f=main():i64{
  let t0=4;
  let t1=t0+1;
  io.println("\(t1)");
  <0
};'
# 6. a name with no trailing digit keeps its spelling in the IR.
printf '%s\n' 'm=c1921f;
f=g(total:i64):i64{<total+1};
f=main():i64{<g(1)};' > "${WORK}/plain.tk"
if "${TKC}" --emit-llvm --out "${WORK}/plain.ll" "${WORK}/plain.tk" >/dev/null 2>&1 &&
   grep -q '%total = alloca i64' "${WORK}/plain.ll"; then
    pass "plain: a parameter 'total' is still %total"
else
    fail "plain: %total not found in the IR"
fi

echo "C1921: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
