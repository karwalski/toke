#!/usr/bin/env bash
# C2299_combinator_arity_and_fnref_call_unchanged.sh — what 127.548 must NOT
# change: the checker's combinator arities, and the toke-side call through a
# function reference.
#
# 127.548 gave the combinator glue one argument the call does not spell
# (tk_arr_map/filter/sort take (arr, adapter, value), tk_arr_reduce
# (arr, init, adapter, value)).  The checker judges receiver-method arity
# from the glue declarations (stdlib_tables.c recv_method_arity), so without
# recv_glue_hidden_args every correct `xs.filter(&p)` would be rejected E4026
# "takes 3 arguments".  And the adapter is used only where a value is handed
# to the runtime: `let g=&pos; g(3)` stays a direct `call fastcc i1`, and a
# module with no combinator call gets no adapter.
#
# WHAT THIS CHECKS:
#   1. `--check` is clean on xs.map(&f) / filter / reduce / fold / sort;
#   2. wrong arities are still E4026 with the arities the CALL spells:
#      xs.filter() expected 2 got 1; xs.reduce(&add) expected 3 got 2;
#      xs.map(&pos;1) expected 2 got 3;
#   3. `let g=&pos; g(3)` lowers to `call fastcc i1 %tN(i64 ...)` through
#      the stored address, and the module defines no @__tk_fvc_ adapter.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2299_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2299: combinator arities and the toke-side fn-ref call are unchanged (127.548)"
echo "-------------------------------------------------------------------------------"

cat > ok.tk <<'TOKE'
m=ok;
f=pos(x:i64):bool{<x>0};
f=dbl(x:i64):i64{<x*2};
f=add(a:i64;b:i64):i64{<a+b};
f=main():i64{
  let xs=@(1;2);
  let a=xs.map(&dbl);
  let b=xs.filter(&pos);
  let c=xs.reduce(0;&add);
  let d=xs.fold(0;&add);
  let e=xs.sort(&add);
  <a.len+b.len+c+d+e.len
};
TOKE
if "${TKC}" --check ok.tk >ok.err 2>&1; then pass "--check clean on map/filter/reduce/fold/sort"
else fail "--check rejected correct calls: $(head -c 400 ok.err)"; fi

cat > bad.tk <<'TOKE'
m=bad;
f=pos(x:i64):bool{<x>0};
f=add(a:i64;b:i64):i64{<a+b};
f=main():i64{
  let xs=@(1;2);
  let a=xs.filter();
  let b=xs.reduce(&add);
  let c=xs.map(&pos;1);
  <0
};
TOKE
"${TKC}" --check bad.tk >bad.json 2>&1
r=$(python3 - bad.json <<'PY'
import json, sys
got = []
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith("{"): continue
    d = json.loads(line)
    if d.get("error_code") == "E4026":
        got.append((d["pos"]["line"], d.get("expected"), d.get("got")))
want = [(6, "2", "1"), (7, "3", "2"), (8, "2", "3")]
print("OK" if got == want else "got %r want %r" % (got, want))
PY
)
[ "$r" = OK ] && pass "E4026 arities are the call's (2/1, 3/2, 2/3)" || fail "E4026: $r"

cat > g.tk <<'TOKE'
m=g;
i=io:std.io;
f=pos(x:i64):bool{<x>0};
f=main():i64{
  let g=&pos;
  if(g(3)){io.println("t")}el{io.println("f")};
  <0
};
TOKE
if "${TKC}" --emit-llvm --out g.ll g.tk >g.err 2>&1; then
  if grep -Eq 'call fastcc i1 %t[0-9]+\(i64 %t[0-9]+\)' g.ll && ! grep -q '__tk_fvc_' g.ll; then
    pass "let g=&pos; g(3) is a direct fastcc i1 call; no adapter emitted"
  else fail "toke-side fn-ref call changed: $(grep -E 'call .*i1|__tk_fvc_' g.ll | head -3)"; fi
else fail "emit-llvm failed: $(head -c 300 g.err)"; fi

echo ""
echo "C2299: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
