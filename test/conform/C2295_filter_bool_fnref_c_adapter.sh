#!/usr/bin/env bash
# C2295_filter_bool_fnref_c_adapter.sh — a bool function handed to
# `.filter` reaches the C runtime through a C-ABI adapter that returns a
# well-defined i64 (story 127.548).
#
# THE DEFECT (P1, silent miscompile on x86-64; CI run 45433635073,
# diff-codegen on test/standalone/test_127_44.tk): `f=pos(x:i64):bool{<x>0}`
# is `define fastcc i1 @..pos(i64)`, and `xs.filter(&pos)` handed
# `ptrtoint i1 (i64)* @..pos to i64` to tk_arr_filter, which called it as
# `int64_t (*)(int64_t)`.  x86-64 defines only bit 0 of the i1 return
# register, so `@(0-3;7;0;9).filter(&pos)` gave [-3,7,9] on linux/amd64.
# arm64 happened to clear the register, which is why this Mac cannot show
# the runtime symptom and these checks read the IR.
#
# THE FIX: the runtime takes (adapter, value).  The adapter
#   define internal i64 @__tk_fvc_<key>(i64 %fv, i64 %a0)
# is C convention (no fastcc), calls the toke function fastcc as the
# toke-side indirect call does, and zero-extends the i1 result.
#
# WHAT THIS CHECKS (host IR, and IR for --target x86_64-unknown-linux-gnu):
#   1. tk_arr_filter is declared (i64, i64, i64) and called with 3 args;
#   2. its 2nd argument is `ptrtoint i64 (i64, i64)* @__tk_fvc_*`, never a
#      bare `ptrtoint i1 (...)` of the user function, which goes 3rd, as
#      the value the adapter calls;
#   3. that adapter is defined without fastcc, returns i64, calls the
#      function `fastcc i1`, and contains `zext i1 ... to i64`;
#   4. the program runs on the host and prints [7,9].
#
# Negative control: the v3.2.0 compiler (main 896f6bdb) fails 1-3.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2295_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2295: a bool fn-ref handed to .filter goes through a C-ABI i64 adapter (127.548)"
echo "-------------------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
f=pos(x:i64):bool{<x>0};
f=main():i64{
  io.println(j.enc(@(0-3;7;0;9).filter(&pos)));
  <0
};
TOKE

check_ir() {   # $1 = label, $2 = .ll
  python3 - "$2" <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
bad = []
if "declare i64 @tk_arr_filter(i64, i64, i64)" not in ir:
    bad.append("tk_arr_filter not declared (i64, i64, i64)")
calls = re.findall(r"call i64 @tk_arr_filter\(([^)]*)\)", ir)
if len(calls) != 1:
    bad.append("expected one tk_arr_filter call, found %d" % len(calls))
else:
    args = [a.strip() for a in calls[0].split(",")]
    if len(args) != 3:
        bad.append("tk_arr_filter called with %d args" % len(args))
    else:
        reg = args[1].split()[-1]
        m = re.search(re.escape(reg) + r" = ptrtoint (.*?) to i64", ir)
        if not m:
            bad.append("2nd argument %s is not a ptrtoint" % reg)
        elif not re.fullmatch(r"i64 \(i64, i64\)\* @__tk_fvc_\w+", m.group(1)):
            bad.append("2nd argument is %r, not a C-ABI adapter" % m.group(1))
        else:
            sym = m.group(1).split("@")[1]
            d = re.search(r"^define ([^\n]*?) @" + sym + r"\(([^\n]*?)\)[^{\n]*\{(.*?)^\}", ir, re.M | re.S)
            if not d:
                bad.append("adapter %s not defined" % sym)
            else:
                head, params, body = d.groups()
                if "fastcc" in head: bad.append("adapter is fastcc: " + head)
                if not head.endswith("i64"): bad.append("adapter does not return i64: " + head)
                if params != "i64 %fv, i64 %a0": bad.append("adapter params: " + params)
                if not re.search(r"call fastcc i1 %\w+\(i64 %p0\)", body):
                    bad.append("adapter does not call the fn fastcc i1")
                if not re.search(r"zext i1 %\w+ to i64", body):
                    bad.append("adapter has no zext i1 -> i64")
        fv = args[2].split()[-1]
        if not re.search(re.escape(fv) + r" = ptrtoint i1 \(i64\)\* @p_pos to i64", ir):
            bad.append("3rd argument %s is not the function value &pos" % fv)
print("\n".join(bad) if bad else "OK")
sys.exit(1 if bad else 0)
PY
}

if "${TKC}" --emit-llvm --out host.ll p.tk >host.err 2>&1; then
  r=$(check_ir host host.ll); [ "$r" = OK ] && pass "host IR: filter(&pos) gets a C-ABI zext adapter" || fail "host IR: $r"
else fail "emit-llvm (host) failed: $(head -c 300 host.err)"; fi

if "${TKC}" --target x86_64-unknown-linux-gnu --emit-llvm --out x86.ll p.tk >x86.err 2>&1; then
  r=$(check_ir x86 x86.ll); [ "$r" = OK ] && pass "x86_64 IR: filter(&pos) gets a C-ABI zext adapter" || fail "x86_64 IR: $r"
else fail "emit-llvm (x86_64) failed: $(head -c 300 x86.err)"; fi

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  [ "$rc" -eq 0 ] && [ "$out" = "[7,9]" ] && pass "runs: [7,9]" || fail "runs: rc=$rc out=$out"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2295: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
