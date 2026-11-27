#!/usr/bin/env bash
# C2311_reduce_bool_str_record_i64_result_type.sh — `.reduce` / `.fold`
# typed from the accumulator function's declared return for bool, str and
# record accumulators, an f64 accumulator over an i64 array, and i64 and
# untyped closures unchanged (story 127.551).
#
# THE DEFECT: the untyped result read a bool accumulator as 0/1
# (`"\(ns.reduce(false;&any))"` printed 1, j.enc printed 1), j.enc of a
# str accumulator printed the pointer and of a record printed its address.
#
# THE FIX: types.c recv_reduce_result_type (the str / record rtype gives the
# emitter its "$str" / record tag via type_backend_tag); llvm.c narrows a
# bool accumulator to i1 on the rtype.
#
# WHAT THIS CHECKS (runs): bool (interpolation, j.enc, `==`), str
# (interpolation, j.enc, `==`), record (field reads, j.enc), an f64
# accumulator over an @i64, i64 (unchanged), and a closure with no declared
# return (unmodelled, still i64).
#
# Negative control: main 12cc40a3 prints 1 for the bool, pointers for the
# str, an address for the record j.enc and 4613937818241073152 for the mixed
# accumulator.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2311_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2311: .reduce/.fold bool/str/record/i64 accumulators (127.551)"
echo "---------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
i=s:std.str;
t=$pt{x:i64;y:f64};
f=any(a:bool;b:i64):bool{<a||b>1};
f=cat(a:str;b:str):str{<s.concat(a;b)};
f=mx(a:f64;b:i64):f64{if(a>2.0){<a};<a+1.5};
f=acc(a:$pt;b:i64):$pt{<$pt{x:a.x+b;y:a.y+0.5}};
f=sum(a:i64;b:i64):i64{<a+b};
f=main():i64{
  let ns=@(1;2;3);
  io.println("bool=\(ns.reduce(false;&any))");
  io.println("benc=\(j.enc(ns.fold(false;&any)))");
  let b=ns.reduce(false;&any);
  if(b==true){io.println("beq ok")}el{io.println("beq BAD")};
  let ss=@("a";"b";"c");
  io.println("str=\(ss.reduce(">";&cat))");
  io.println("senc=\(j.enc(ss.fold("";&cat)))");
  let t=ss.fold("";&cat);
  if(t=="abc"){io.println("seq ok")}el{io.println("seq BAD")};
  let r=ns.reduce($pt{x:0;y:0.0};&acc);
  io.println("rec=\(r.x),\(r.y)");
  io.println("renc=\(j.enc(ns.fold($pt{x:1;y:1.0};&acc)))");
  io.println("mixed=\(ns.reduce(0.0;&mx))");
  io.println("i64=\(ns.reduce(0;&sum))");
  io.println("untyped=\(ns.reduce(0;fn(a:i64;b:i64){<a+b}))");
  <0
};
TOKE

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want=$'bool=true\nbenc=true\nbeq ok\nstr=>abc\nsenc="abc"\nseq ok\nrec=6,1.5\nrenc={"x":7,"y":2.5}\nmixed=3\ni64=6\nuntyped=6'
  if [ "$rc" -eq 0 ] && [ "$out" = "$want" ]; then
    pass "bool/str/record/mixed accumulators keep their type; i64 and untyped unchanged"
  else
    fail "rc=$rc out=$(echo "$out" | tr '\n' '|')"
  fi
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2311: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
