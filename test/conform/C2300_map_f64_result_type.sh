#!/usr/bin/env bash
# C2300_map_f64_result_type.sh — `xs.map(f)` with an f64-returning function
# is an @f64: its elements encode and interpolate as doubles (story 127.550).
#
# THE DEFECT: since 127.548 `@(1.5;2.5).map(&dbl)` (dbl:(f64):f64) stores
# [3.0,5.0], but the result was typed @i64.  The checker did not model
# `.map` (types.c recv_result_on), and the emitter's fallback tag
# (llvm.c expr_struct_type, 127.44) looked the function up by its bare name
# `dbl` while the symbol is `p_dbl`, so it always answered "@i64".
# `j.enc(fs.map(&dbl))` printed [4613937818241073152,4617315517961601024] and
# `"\(ds.get(0))"` printed 4613937818241073152.
#
# THE FIX: types.c recv_map_result_type types `.map` on an established array
# as @(<return of the function value's signature>); llvm.c resolves the
# mapped `&f` through fvc_ref_sig (the mangled symbol) for the fallback tag.
#
# WHAT THIS CHECKS (runs): the f64 map through `&dbl`, a `let g=&dbl` value
# and a `(f64):f64` parameter, as j.enc, as interpolation of `.get(i)` on a
# `let` binding and in place, and compared with an f64 literal.
#
# Negative control: main 58bd74e8 prints the bit patterns (only the @f64
# parameter line, typed by ap's declared return, is right).
#
# Story: 127.550
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2300_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2300: .map over an f64 function is typed @f64 (127.550)"
echo "--------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
f=dbl(x:f64):f64{<x*2.0};
f=ap(xs:@f64;g:(f64):f64):@f64{<xs.map(g)};
f=main():i64{
  let fs=@(1.5;2.5;0.25);
  io.println(j.enc(fs.map(&dbl)));
  let ds=fs.map(&dbl);
  let d0=ds.get(0);
  io.println("d0=\(d0)");
  io.println("d2=\(fs.map(&dbl).get(2))");
  if(ds.get(1)==5.0){io.println("cmp ok")}el{io.println("cmp BAD")};
  let g=&dbl;
  io.println(j.enc(fs.map(g)));
  io.println(j.enc(ap(fs;&dbl)));
  <0
};
TOKE

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want=$'[3,5,0.5]\nd0=3\nd2=0.5\ncmp ok\n[3,5,0.5]\n[3,5,0.5]'
  if [ "$rc" -eq 0 ] && [ "$out" = "$want" ]; then
    pass "f64 map encodes, interpolates and compares as doubles"
  else
    fail "rc=$rc out=$(echo "$out" | tr '\n' '|')"
  fi
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2300: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
