#!/usr/bin/env bash
# C2296_map_bool_f64_fn_adapter.sh — `.map` over a bool or an f64 function
# hands the runtime a C-ABI adapter that converts both ways (story 127.548).
#
# THE DEFECT: tk_arr_map called the toke function as `int64_t (*)(int64_t)`.
#   - `xs.map(&pos)` with pos:(i64):bool stored an i1 return register whose
#     bits above bit 0 x86-64 leaves undefined;
#   - `fs.map(&dbl)` with dbl:(f64):f64 passed the element's bits in an
#     integer register while dbl read its argument from a float register, and
#     stored whatever the integer return register held — wrong on EVERY
#     target (on arm64 main 896f6bdb, `@(1.5;2.5).map(&dbl)` returned the
#     input bits 1.5, 2.5 unchanged).
#
# THE FIX: the adapter narrows each i64 slot to the parameter type (bitcast
# i64 -> double for f64) and widens the result (zext i1, bitcast double ->
# i64), calling the function fastcc as the toke-side call does.
#
# WHAT THIS CHECKS:
#   1. map(&pos): adapter defined without fastcc, `call fastcc i1`, `zext i1`;
#   2. map(&dbl): adapter has `bitcast i64 %a0 to double`, `call fastcc
#      double`, `bitcast double ... to i64`;
#   3. runs: map(&pos) -> [false,true,false,true] (an @bool since 127.550;
#      127.548 recorded the @i64-tagged [0,1,0,1]); map(&dbl) first element
#      == 3.0.
#
# Negative control: main 896f6bdb fails 1-2 and prints "f64 BAD" for 3.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2296_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2296: .map over bool / f64 functions converts through a C-ABI adapter (127.548)"
echo "-------------------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
f=pos(x:i64):bool{<x>0};
f=dbl(x:f64):f64{<x*2.0};
f=f0(a:@f64):f64{<a.get(0)};
f=main():i64{
  io.println(j.enc(@(0-3;7;0;9).map(&pos)));
  let d=f0(@(1.5;2.5).map(&dbl));
  if(d==3.0){io.println("f64 ok")}el{io.println("f64 BAD")};
  <0
};
TOKE

if "${TKC}" --emit-llvm --out p.ll p.tk >emit.err 2>&1; then
  r=$(python3 - p.ll <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
bad = []
def adapter_for(fnsym):
    # the (adapter, value) pair handed to tk_arr_map whose value is &fnsym
    for args in re.findall(r"call i64 @tk_arr_map\(([^)]*)\)", ir):
        a = [x.strip().split()[-1] for x in args.split(",")]
        if len(a) != 3: continue
        v = re.search(re.escape(a[2]) + r" = ptrtoint [^\n]*@" + fnsym + r" to i64", ir)
        f = re.search(re.escape(a[1]) + r" = ptrtoint i64 \(i64, i64\)\* @(__tk_fvc_\w+) to i64", ir)
        if v and f: return f.group(1)
    return None
def body(sym):
    d = re.search(r"^define ([^\n]*?) @" + sym + r"\(([^\n]*?)\)[^{\n]*\{(.*?)^\}", ir, re.M | re.S)
    return d.groups() if d else None
for fn, need in (("p_pos", [r"call fastcc i1 %\w+\(i64 %p0\)", r"zext i1 %\w+ to i64"]),
                 ("p_dbl", [r"bitcast i64 %a0 to double", r"call fastcc double %\w+\(double %p0\)",
                            r"bitcast double %\w+ to i64"])):
    sym = adapter_for(fn)
    if not sym: bad.append("map(&%s): no (adapter, value) pair handed to tk_arr_map" % fn); continue
    b = body(sym)
    if not b: bad.append("adapter %s not defined" % sym); continue
    head, params, text = b
    if "fastcc" in head or not head.endswith("i64"):
        bad.append("adapter %s head %r is not C-convention i64" % (sym, head))
    for pat in need:
        if not re.search(pat, text): bad.append("adapter %s lacks %s" % (sym, pat))
print("\n".join(bad) if bad else "OK")
PY
)
  [ "$r" = OK ] && pass "IR: map(&pos) zext adapter, map(&dbl) bitcast adapter" || fail "IR: $r"
else fail "emit-llvm failed: $(head -c 300 emit.err)"; fi

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want=$'[false,true,false,true]\nf64 ok'
  [ "$rc" -eq 0 ] && [ "$out" = "$want" ] && pass "runs: [false,true,false,true] and f64 3.0" || fail "runs: rc=$rc out=$out"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2296: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
