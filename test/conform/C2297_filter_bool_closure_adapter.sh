#!/usr/bin/env bash
# C2297_filter_bool_closure_adapter.sh — a closure handed to `.filter` /
# `.map` is called as a closure, through the C-ABI adapter (story 127.548).
#
# THE DEFECT: a closure value is its env box + 1 (124.0c).  The combinators
# passed it to the runtime as if it were a function address and C jumped to
# it: `xs.filter(fn(x:i64):bool{<x>0})` died with SIGBUS on arm64 at main
# 896f6bdb (and its bool result would have had the x86-64 i1 problem of
# C2295 had it got that far).
#
# THE FIX: the adapter tests the low bit exactly as the toke-side
# function-value call does: set -> load env[0] and call it fastcc with env;
# clear -> call the address.  A closure literal handed to the combinator is
# released after the call (it was leaked before).
#
# WHAT THIS CHECKS:
#   1. IR: the value handed to tk_arr_filter for a closure literal is the
#      closure box (ptrtoint i8*), the adapter has a closure branch
#      `call fastcc i1 %..(i8* %env, i64 %p0)` and zexts, and the literal is
#      released (tk_clos_release) after the call;
#   2. runs: literal, capturing literal, closure-bound local (twice: it is
#      borrowed, not consumed), a closure through a (i64):bool parameter,
#      and `.map` of a capturing i64 closure.
#
# Negative control: main 896f6bdb fails 1 and dies (SIGBUS, exit 138) in 2.
#
# Story: 127.548
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2297_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2297: closures handed to .filter / .map are called as closures (127.548)"
echo "-----------------------------------------------------------------------"

cat > ir.tk <<'TOKE'
m=q;
i=io:std.io;
i=j:std.json;
f=main():i64{
  io.println(j.enc(@(0-3;7;0;9).filter(fn(x:i64):bool{<x>0})));
  <0
};
TOKE

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
f=keep(xs:@i64;p:(i64):bool):@i64{<xs.filter(p)};
f=main():i64{
  let xs=@(0-3;7;0;9);
  io.println(j.enc(xs.filter(fn(x:i64):bool{<x>0})));
  let k=5;
  io.println(j.enc(xs.filter(fn(x:i64):bool{<x>k})));
  let big=fn(x:i64):bool{<x>k};
  io.println(j.enc(xs.filter(big)));
  io.println(j.enc(xs.filter(big)));
  io.println(j.enc(keep(xs;fn(x:i64):bool{<x<0})));
  io.println(j.enc(xs.map(fn(x:i64):i64{<x+k})));
  <0
};
TOKE

if "${TKC}" --emit-llvm --out ir.ll ir.tk >emit.err 2>&1; then
  r=$(python3 - ir.ll <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
bad = []
calls = re.findall(r"(%t\d+) = call i64 @tk_arr_filter\(([^)]*)\)", ir)
if len(calls) != 1:
    bad.append("expected one tk_arr_filter call, found %d" % len(calls))
else:
    res, args = calls[0]
    a = [x.strip().split()[-1] for x in args.split(",")]
    if len(a) != 3:
        bad.append("tk_arr_filter called with %d args" % len(a))
    else:
        if not re.search(re.escape(a[2]) + r" = ptrtoint i8\* %t\d+ to i64", ir):
            bad.append("3rd argument is not the closure box")
        f = re.search(re.escape(a[1]) + r" = ptrtoint i64 \(i64, i64\)\* @(__tk_fvc_\w+) to i64", ir)
        if not f:
            bad.append("2nd argument is not a C-ABI adapter")
        else:
            d = re.search(r"^define ([^\n]*?) @" + f.group(1) + r"\(([^\n]*?)\)[^{\n]*\{(.*?)^\}", ir, re.M | re.S)
            if not d: bad.append("adapter not defined")
            else:
                head, params, body = d.groups()
                if "fastcc" in head: bad.append("adapter is fastcc")
                if not re.search(r"and i64 %fv, 1", body): bad.append("adapter does not test the closure tag")
                if not re.search(r"call fastcc i1 %\w+\(i8\* %env, i64 %p0\)", body):
                    bad.append("adapter has no closure call with env")
                if len(re.findall(r"zext i1 %\w+ to i64", body)) < 2:
                    bad.append("adapter does not zext both branches")
        after = ir.split(res + " = call i64 @tk_arr_filter", 1)[1].split("\n}", 1)[0]
        if "@tk_clos_release" not in after:
            bad.append("the closure literal is not released after the call")
print("\n".join(bad) if bad else "OK")
PY
)
  [ "$r" = OK ] && pass "IR: closure box + adapter with a closure branch; literal released" || fail "IR: $r"
else fail "emit-llvm failed: $(head -c 300 emit.err)"; fi

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want=$'[7,9]\n[7,9]\n[7,9]\n[7,9]\n[-3]\n[2,12,5,14]'
  [ "$rc" -eq 0 ] && [ "$out" = "$want" ] && pass "runs: literal, capture, local x2, fn param, map" || fail "runs: rc=$rc out=$(echo $out)"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2297: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
