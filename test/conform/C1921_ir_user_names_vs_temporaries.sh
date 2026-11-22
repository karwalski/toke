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
# keeps its spelling.  Cases 7-12 drive the name-keyed fast paths (map
# in-place set, array append/set, x=x+@(..), str accumulators, a str
# capture) with digit-ending bindings, and case 13 checks plain and
# digit-suffixed spellings of one program lower to the same IR.
#
# Negative control: v3.2.0 (ea7ddf22) — params, labels and loop fail to
# build ("multiple definition of local value named 't0'", "'%svarm3' is not
# a basic block"); `control` builds there too, and so does `closure`, only
# because a lifted body's temporaries happen to be numbered past t0/t3 there.
# mapset_t1 traps with RT003 on v3.2.0 and on base b518b41c (114.15's t<N>
# aliasing); at round-1 61725c51 mapset trapped too and the differential
# failed (the map set, and zz9's capture slot typed i64).
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

# 7-12. Round 2 (reviewer finding on 61725c51): aliasing every digit-ending
#    name (m1 -> %m1.N) left registry lookups made with the toke spelling --
#    the map tag, the local's type, a capture's type -- missing, so
#    `m1=m1.set(k;v)` lowered as an ARRAY in-place set and trapped with
#    RT003 (and so did `t1` on 3.2.0, under 114.15's aliasing).  Each case
#    exercises one name-keyed fast path with a digit-ending binding.
run mapset '40|' 'm=c1921g;
i=io:std.io;
f=main():i64{
  let m1=mut.@("":0);
  m1=m1.set("a";40);
  io.println("\(mt m1.get("a"){$ok:v v;$err:e 0})");
  <0
};'
run mapset_t1 '40|' 'm=c1921h;
i=io:std.io;
f=main():i64{
  let t1=mut.@("":0);
  t1=t1.set("a";40);
  io.println("\(mt t1.get("a"){$ok:v v;$err:e 0})");
  <0
};'
# appends 0..4 then set(0;7): 7+1+2+3+4+0 = 17 over 6 elements.
run arrappend '17 6|' 'm=c1921i;
i=io:std.io;
f=main():i64{
  let xs1=mut.@(0);
  lp(let k=0;k<5;k=k+1){ xs1=xs1.append(k) };
  xs1=xs1.set(0;7);
  let tot2=mut.0;
  lp(let j=0;j<xs1.len;j=j+1){ tot2=tot2+xs1.get(j) };
  io.println("\(tot2) \(xs1.len)");
  <0
};'
# x=x+@(..) in place (127.229): 1 + 3*2 = 7 elements, the last is 3.
run selfconcat '7 3|' 'm=c1921j;
i=io:std.io;
f=main():i64{
  let xs2=mut.@(0);
  lp(let k=1;k<4;k=k+1){ xs2=xs2+@(k;k) };
  io.println("\(xs2.len) \(xs2.get(6))");
  <0
};'
# a str accumulator, and an array proven to hold strings (126.8 @str tag).
run strs 'ababab|w=p|w=q|' 'm=c1921k;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let sb1=mut."";
  lp(let k=0;k<3;k=k+1){ sb1=str.concat(sb1;"ab") };
  io.println(sb1);
  let ws2=mut.@();
  ws2=ws2.append("p");
  ws2=ws2.append("q");
  lp(let k=0;k<ws2.len;k=k+1){ io.println("w=\(ws2.get(k))") };
  <0
};'
# a captured str local keeps its i8* slot in the lifted closure.
run strcapture 'hey|8|' 'm=c1921l;
i=io:std.io;
f=main():i64{
  let zz9="hey";
  let nm9=7;
  let g=fn(q:i64):i64{ io.println(zz9); <q+nm9 };
  io.println("\(g(1))");
  <0
};'
# 13. differential: one program written with plain names and with every
#     binding digit-suffixed lowers to the SAME IR once the names are
#     normalised (%name9.N / %name -> %name).  Any registry lookup that
#     misses for the aliased spelling changes the lowering and shows here.
plainsrc='m=c1921m;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let mp=mut.@("":0);
  mp=mp.set("a";40);
  let xs=mut.@(0);
  lp(let k=0;k<3;k=k+1){ xs=xs.append(k) };
  xs=xs.set(0;5);
  xs=xs+@(9);
  let sb=mut."";
  sb=str.concat(sb;"ab");
  let zz="hey";
  let nm=7;
  let g=fn(q:i64):i64{ io.println(zz); <q+nm };
  io.println("\(mt mp.get("a"){$ok:v v;$err:e 0}) \(xs.len) \(sb) \(g(1))");
  <0
};'
digitsrc="$(printf '%s' "${plainsrc}" | sed -E 's/(^|[^A-Za-z.])(mp|xs|sb|zz|nm)([^A-Za-z0-9]|$)/\1\29\3/g; s/(^|[^A-Za-z.])(mp|xs|sb|zz|nm)([^A-Za-z0-9]|$)/\1\29\3/g; s/mut\.(mp|xs|sb|zz|nm)([^0-9])/mut.\19\2/g')"
printf '%s\n' "${plainsrc}" > "${WORK}/dplain.tk"
printf '%s\n' "${digitsrc}" > "${WORK}/ddigit.tk"
if ! grep -q 'mp9=mp9.set' "${WORK}/ddigit.tk"; then
    fail "differential: the digit-suffixed copy was not produced"
elif "${TKC}" --emit-llvm --out "${WORK}/dplain.ll" "${WORK}/dplain.tk" >/dev/null 2>&1 &&
     "${TKC}" --emit-llvm --out "${WORK}/ddigit.ll" "${WORK}/ddigit.tk" >/dev/null 2>&1; then
    norm() {
        python3 - "$1" <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
ir = re.sub(r'%(mp|xs|sb|zz|nm)9?(\.\d+)?(?![\w.])', r'%\1', ir)
ir = re.sub(r'^; ModuleID.*$|^source_filename.*$', '', ir, flags=re.M)
sys.stdout.write(ir)
PY
    }
    if [ "$(norm "${WORK}/dplain.ll")" = "$(norm "${WORK}/ddigit.ll")" ]; then
        pass "differential: plain and digit-suffixed names lower to the same IR"
    else
        fail "differential: the IR differs once names are normalised"
        diff <(norm "${WORK}/dplain.ll") <(norm "${WORK}/ddigit.ll") | head -10
    fi
else
    fail "differential: --emit-llvm failed"
fi

echo "C1921: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
