#!/usr/bin/env bash
# C2310_reduce_f64_result_type.sh — `xs.reduce(init;f)` / `xs.fold(init;f)`
# with an f64 accumulator is an f64: it interpolates, encodes, compares and
# does arithmetic as a double (story 127.551).
#
# THE DEFECT: since 127.548 `@(1.5;2.5;0.25).reduce(0.0;&add)`
# (add:(f64;f64):f64) computes 4.25, but the checker did not model
# `.reduce` / `.fold` (types.c recv_result_on; 127.550 added only `.map`),
# so the call was untyped and the emitter's i64 default read the double's
# bits: `"\(fs.reduce(0.0;&add))"` printed 4616471093031469056, as did a
# `let` copy, j.enc of it, and `==4.25` was false.
#
# THE FIX: types.c recv_reduce_result_type types the call as the declared
# return of the accumulator function value (a two-parameter signature, read
# by recv_map_fn_sig); llvm.c bitcasts tk_arr_reduce's i64 to double on that
# rtype (and expr_llvm_type answers "double"), as 127.320 does for getor.
#
# WHAT THIS CHECKS (runs): an f64 reduce through `&add`, `.fold`, a
# `let g=&add` value, a closure literal, a capturing closure, a `let`-bound
# closure and a `(f64;f64):f64` parameter; interpolated in place and through
# a `let`, j.enc, `==`, `+`, `>`, as a call argument and as a return value.
#
# Negative control: main 12cc40a3 prints the bit patterns.
#
# Story: 127.551
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2310_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2310: .reduce/.fold with an f64 accumulator is typed f64 (127.551)"
echo "------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
f=add(a:f64;b:f64):f64{<a+b};
f=tot(xs:@f64):f64{<xs.reduce(0.0;&add)};
f=ap(xs:@f64;g:(f64;f64):f64):f64{<xs.fold(0.0;g)};
f=sh(v:f64):f64{<v*10.0};
f=main():i64{
  let fs=@(1.5;2.5;0.25);
  io.println("in=\(fs.reduce(0.0;&add))");
  let s=fs.reduce(0.0;&add);
  io.println("let=\(s)");
  io.println("fold=\(fs.fold(1.0;&add))");
  io.println("enc=\(j.enc(s))");
  if(s==4.25){io.println("eq ok")}el{io.println("eq BAD")};
  if(fs.reduce(0.0;&add)>4.0){io.println("gt ok")}el{io.println("gt BAD")};
  io.println("plus=\(fs.reduce(0.0;&add)+1.0)");
  let g=&add;
  io.println("fnref-local=\(fs.reduce(0.0;g))");
  io.println("closure=\(fs.reduce(0.0;fn(a:f64;b:f64):f64{<a*b+1.0}))");
  let k=0.5;
  io.println("capture=\(fs.fold(0.0;fn(a:f64;b:f64):f64{<a+b*k}))");
  let h=fn(a:f64;b:f64):f64{<a-b};
  io.println("closure-local=\(fs.fold(0.0;h))");
  io.println("param=\(ap(fs;&add))");
  io.println("arg=\(sh(fs.reduce(0.0;&add)))");
  io.println("ret=\(tot(fs))");
  <0
};
TOKE

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want=$'in=4.25\nlet=4.25\nfold=5.25\nenc=4.25\neq ok\ngt ok\nplus=5.25\nfnref-local=4.25\nclosure=1.875\ncapture=2.125\nclosure-local=-4.25\nparam=4.25\narg=42.5\nret=4.25'
  if [ "$rc" -eq 0 ] && [ "$out" = "$want" ]; then
    pass "f64 reduce/fold interpolates, encodes, compares and computes as a double"
  else
    fail "rc=$rc out=$(echo "$out" | tr '\n' '|')"
  fi
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2310: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
