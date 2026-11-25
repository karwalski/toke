#!/usr/bin/env bash
# C2298_reduce_sort_pred_adapter.sh — `.reduce` / `.fold` / `.sort` hand the
# runtime a two-argument C-ABI adapter, and a predicate is NOT masked to its
# low bit (story 127.548).
#
# THE DEFECT: tk_arr_reduce and tk_arr_sort called the toke function as
# `int64_t (*)(int64_t, int64_t)`, the same bare-pointer hand-off as
# filter/map (C2295, C2296): a `(acc:bool;x:i64):bool` reducer returned an
# i1 whose upper bits x86-64 leaves undefined, and a closure comparator was
# called as code.
#
# THE FIX: tk_arr_reduce(arr, init, adapter, value) and
# tk_arr_sort(arr, adapter, value).  The adapter takes (fv, a0, a1), narrows
# a bool parameter with `icmp ne i64 %a0, 0` (any non-zero slot is true),
# and zexts a bool result.  An i64 predicate or comparator result goes back
# untouched: the fix is in the call, not a `& 1` in C, so an i64 predicate
# returning 2 still counts as true.  The sort comparator pair is saved and
# restored around qsort, so a comparator that sorts does not clobber it.
#
# WHAT THIS CHECKS:
#   1. IR: tk_arr_reduce declared (i64, i64, i64, i64), tk_arr_sort
#      (i64, i64, i64); the bool reducer's adapter takes (i64 %fv, i64 %a0,
#      i64 %a1), has `icmp ne i64 %a0, 0` and `zext i1`; the i64 predicate's
#      adapter returns the call result unmasked (no `and` on it);
#   2. runs: bool reduce (true / false), fold, sort by fn-ref, sort by a
#      closure, a comparator that itself sorts, filter by an i64 predicate
#      returning 2.
#
# Negative control: main 896f6bdb fails 1 and dies (SIGBUS) in 2.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2298_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2298: reduce / fold / sort adapters; predicates are not masked (127.548)"
echo "-----------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
f=anybig(acc:bool;x:i64):bool{<acc||x>8};
f=add(a:i64;b:i64):i64{<a+b};
f=desc(a:i64;b:i64):i64{<b-a};
f=two(x:i64):i64{if(x>0){<2};<0};
f=inner(a:i64;b:i64):i64{
  let t=@(3;1;2).sort(&desc);
  <a-b+t.get(0)-3
};
f=main():i64{
  let xs=@(0-3;7;0;9);
  if(xs.reduce(false;&anybig)){io.println("t")}el{io.println("f")};
  if(@(1;2).reduce(false;&anybig)){io.println("t")}el{io.println("f")};
  io.println(j.enc(xs.fold(0;&add)));
  io.println(j.enc(xs.sort(&desc)));
  io.println(j.enc(xs.sort(fn(a:i64;b:i64):i64{<a-b})));
  io.println(j.enc(xs.sort(&inner)));
  io.println(j.enc(xs.filter(&two)));
  <0
};
TOKE

if "${TKC}" --emit-llvm --out p.ll p.tk >emit.err 2>&1; then
  r=$(python3 - p.ll <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
bad = []
for d in ("declare i64 @tk_arr_reduce(i64, i64, i64, i64)", "declare i64 @tk_arr_sort(i64, i64, i64)"):
    if d not in ir: bad.append("missing " + d)
def adapter(callee, fnsym, nargs, fnpos):
    for args in re.findall(r"call i64 @" + callee + r"\(([^)]*)\)", ir):
        a = [x.strip().split()[-1] for x in args.split(",")]
        if len(a) != nargs: continue
        if not re.search(re.escape(a[fnpos + 1]) + r" = ptrtoint [^\n]*@" + fnsym + r" to i64", ir): continue
        f = re.search(re.escape(a[fnpos]) + r" = ptrtoint i64 \([^)]*\)\* @(__tk_fvc_\w+) to i64", ir)
        if f: return f.group(1)
    return None
def body(sym):
    d = re.search(r"^define ([^\n]*?) @" + sym + r"\(([^\n]*?)\)[^{\n]*\{(.*?)^\}", ir, re.M | re.S)
    return d.groups() if d else None
s = adapter("tk_arr_reduce", "p_anybig", 4, 2)
b = body(s) if s else None
if not b: bad.append("reduce(&anybig): no adapter pair")
else:
    head, params, text = b
    if "fastcc" in head or params != "i64 %fv, i64 %a0, i64 %a1": bad.append("reduce adapter head/params: %s (%s)" % (head, params))
    if "icmp ne i64 %a0, 0" not in text: bad.append("reduce adapter does not test the bool acc slot != 0")
    if not re.search(r"zext i1 %\w+ to i64", text): bad.append("reduce adapter has no zext")
s = adapter("tk_arr_sort", "p_desc", 3, 1)
if not s or not body(s): bad.append("sort(&desc): no adapter pair")
s = adapter("tk_arr_filter", "p_two", 3, 1)
b = body(s) if s else None
if not b: bad.append("filter(&two): no adapter pair")
else:
    text = b[2]
    if re.search(r"\band i64 %c", text) or "zext" in text or "trunc" in text:
        bad.append("i64 predicate result is altered in the adapter")
    if not re.search(r"ret i64 %c2\b", text): bad.append("i64 predicate result not returned as is")
print("\n".join(bad) if bad else "OK")
PY
)
  [ "$r" = OK ] && pass "IR: reduce/sort adapters; bool acc narrowed != 0; i64 result unmasked" || fail "IR: $r"
else fail "emit-llvm failed: $(head -c 300 emit.err)"; fi

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want=$'t\nf\n13\n[9,7,0,-3]\n[-3,0,7,9]\n[-3,0,7,9]\n[7,9]'
  [ "$rc" -eq 0 ] && [ "$out" = "$want" ] && pass "runs: reduce t/f, fold, sort fn/closure/nested, pred 2" || fail "runs: rc=$rc out=$(echo $out)"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2298: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
