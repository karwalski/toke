#!/usr/bin/env bash
# C2312_reduce_result_type_diagnostics.sh — with `.reduce` / `.fold` typed
# (story 127.551) a mismatched use of the result is E4031, and nothing that
# was accepted correctly before gains a diagnostic.
#
# THE DEFECT: the result was unknown to the checker, so
# `let a:i64=fs.reduce(0.0;&add)` and `let s:str=fs.fold(0.0;&add)` passed
# --check and stored or printed the double's bits.
#
# WHAT THIS CHECKS (`tkc --check`, structured JSON):
#   1. E4031 (no `fix`) with exact expected/got for: an i64 / str
#      annotation on an f64 reduce or fold (via &f, a closure literal, a
#      `let g=&f` value); a bool annotation on a `let` copy; a str
#      annotation on a bool reduce;
#   2. clean: a matching f64 annotation, a closure with no declared return
#      (unmodelled); a fallible callee is unmodelled, and since 127.552 the
#      call itself is E4031 (C2314);
#   3. E4026 arity on `.reduce` / `.fold` unchanged.
#
# Negative control: main 12cc40a3 reports nothing for the cases in 1.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2312_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2312: diagnostics on a typed .reduce/.fold result (127.551)"
echo "-------------------------------------------------------------"

# check NAME BODY WANT -- WANT is "clean" or "CODE|expected|got" ("-" = any)
check() {
  local name="$1" body="$2" want="$3"
  printf 'm=p;\ni=io:std.io;\nt=$e{m:str};\nf=add(a:f64;b:f64):f64{<a+b};\nf=bo(a:bool;b:bool):bool{<a||b};\nf=bad(a:f64;b:f64):f64!$e{<a};\nf=main():i64{\n  let fs=@(1.5;2.5);\n  %s\n  <0\n};\n' "$body" > d.tk
  "${TKC}" --check d.tk > d.out 2>&1
  local r
  r=$(python3 - d.out "$want" <<'PY'
import json, sys
recs = []
for line in open(sys.argv[1]):
    line = line.strip()
    if line.startswith("{"):
        try: recs.append(json.loads(line))
        except ValueError: pass
want = sys.argv[2]
if want == "clean":
    print("OK" if not recs else "unexpected: %s" % [r.get("error_code") for r in recs]); sys.exit()
code, exp, got = want.split("|")
if len(recs) != 1: print("want one %s, got %s" % (code, [(r.get("error_code"), r.get("message")) for r in recs])); sys.exit()
r = recs[0]
bad = []
if r.get("error_code") != code: bad.append("code %s" % r.get("error_code"))
if exp != "-" and r.get("expected") != exp: bad.append("expected %r" % r.get("expected"))
if got != "-" and r.get("got") != got: bad.append("got %r" % r.get("got"))
if "fix" in r: bad.append("has fix %r" % r.get("fix"))
print("OK" if not bad else "; ".join(bad))
PY
)
  [ "$r" = OK ] && pass "$name" || fail "$name: $r"
}

check "i64 annotation on &add reduce"   'let a:i64=fs.reduce(0.0;&add);'                      'E4031|i64|f64'
check "str annotation on &add fold"     'let a:str=fs.fold(0.0;&add);'                        'E4031|str|f64'
check "i64 annotation via let g=&add"   'let g=&add; let a:i64=fs.reduce(0.0;g);'             'E4031|i64|f64'
check "i64 annotation on closure reduce" 'let a:i64=fs.reduce(0.0;fn(x:f64;y:f64):f64{<x+y});' 'E4031|i64|f64'
check "bool annotation on a let copy"   'let a=fs.reduce(0.0;&add); let b:bool=a;'            'E4031|bool|f64'
check "str annotation on bool reduce"   'let a:str=@(true).reduce(false;&bo);'                'E4031|str|bool'
check "f64 annotation clean"            'let a:f64=fs.reduce(0.0;&add);'                      'clean'
check "undeclared closure return clean" 'let a:i64=fs.reduce(0.0;fn(x:f64;y:f64){<x+y});'     'clean'
# 127.552: the result stays unmodelled, but the call is now judged: a
# fallible function cannot be an accumulator (C2314).
check "fallible callee: 127.552 E4031"   'let a=fs.reduce(0.0;&bad);'                          'E4031|(f64;f64):f64|(f64;f64):f64!$e'
check "E4026 arity (1) unchanged"       'let a=fs.reduce(0.0);'                               'E4026|-|-'
check "E4026 arity (3) unchanged"       'let a=fs.fold(0.0;&add;1);'                          'E4026|-|-'

echo ""
echo "C2312: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
