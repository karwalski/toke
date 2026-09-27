#!/usr/bin/env bash
# C075_match_on_indexed_sum.sh — a match on a sum value read out of an array
# dispatches on its tag, because match lowering reads the checker's type
# (story 127.174).
#
# THE DEFECT. Since 886bdb0 the checker types `xs.get(i)` on an @($shape) as
# the $shape, and accepts an exhaustive match over it. The emitter's match
# lowering decided "is this a sum?" from its own local tags only, and those do
# not follow a value out of an array. Measured at main 2e932d6:
#
#   3-arm match on xs.get(0)          --check 0, build E9005 "sum type ... not
#                                     established" (check-clean, build-fail)
#   2-arm match on xs.get(0) over a   --check 0, build 0, prints
#   2-variant sum with a str payload  "a4369701536 a4369701552": the binary
#                                     $ok/$err lowering took arm 0 for both
#                                     values and bound the box address
#
# The second is silent wrong output and is the worse of the two.
#
# THE RULE. When the backend's tags do not resolve the scrutinee to a sum,
# the checker's type on the scrutinee node does; the box layout is the same
# however the value arrived. Every case asserts every variant (a fallback to
# the binary lowering would still get the first arm right) at -O0 and -O2.
#
# Negative control: TKC=<main 2e932d6 build> — 1 and 3 fail to compile
# (E9005), 2 prints box addresses.
#
# Story: 127.174

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC}"; exit 1; fi
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

echo "C075: a match on an indexed sum value dispatches on its tag"
echo "-----------------------------------------------------------"

runs() {
    local name="$1" expected="$2" src="$3" o out rc
    printf '%s\n' "${src}" > "${WORK}/p.tk"
    for o in -O0 -O2; do
        rm -f "${WORK}/p_bin"; rc=0
        if ! "${TKC}" --allow-all "${o}" --out "${WORK}/p_bin" "${WORK}/p.tk" > "${WORK}/c.log" 2>&1; then
            echo "FAIL ${name} ${o}: compile failed"
            awk 'match($0,/"error_code":"[A-Z][0-9]+"/){print "        " substr($0,RSTART,RLENGTH)}' "${WORK}/c.log" | head -1
            FAIL=$((FAIL + 1)); continue
        fi
        out="$("${WORK}/p_bin" 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then echo "PASS ${name} ${o}"; PASS=$((PASS + 1))
        else echo "FAIL ${name} ${o}"; echo "        expected: ${expected}"; echo "        got:      ${out} (rc ${rc})"; FAIL=$((FAIL + 1)); fi
    done
}

SHAPE='m=main;
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

runs "1-3-arm-direct-and-bound" "11 22 33" "${SHAPE}
f=main():i64{
  let xs=@(mk(0);mk(1);mk(2));
  let a=xs.get(0);
  let r=mt(a){\$circle:x x+10;\$square:x x+20;\$tri:x x+30};
  let q=mt(xs.get(1)){\$circle:x x+10;\$square:x x+20;\$tri:x x+30};
  let z=mt(xs.get(2)){\$circle:x x+10;\$square:x x+20;\$tri:x x+30};
  io.println(\"\\(r) \\(q) \\(z)\");
  <0
};"

runs "2-2-arm-str-payload" "a7 bhi" 'm=main;
i=io:std.io;
t=$pr{$a:i64;$b:str};
f=mk(k:i64):$pr{
  if(k==0){
    <$a(7)
  };
  <$b("hi")
};
f=main():i64{
  let xs=@(mk(0);mk(1));
  let r=mt(xs.get(0)){$a:x "a\(x)";$b:s "b\(s)"};
  let q=mt(xs.get(1)){$a:x "a\(x)";$b:s "b\(s)"};
  io.println("\(r) \(q)");
  <0
};'

runs "3-in-a-loop-over-the-array" "1 2 3 " "${SHAPE}
f=main():i64{
  let xs=@(mk(0);mk(1);mk(2));
  lp(let i=0;i<xs.len;i=i+1){
    let v=mt(xs.get(i)){\$circle:x x;\$square:x x;\$tri:x x};
    io.print(\"\\(v) \");
  };
  io.println(\"\");
  <0
};"

echo "-----------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
