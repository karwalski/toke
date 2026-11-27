#!/usr/bin/env bash
# C2304_map_result_type_diagnostics.sh — with `.map` typed (story 127.550) a
# mismatched use of its result is E4031, and nothing that was accepted
# correctly before gains a diagnostic.
#
# THE DEFECT: the result was unknown to the checker, so `let a:@i64=
# fs.map(&dbl)` and `let s:str=fs.map(&dbl).get(0)` passed --check and
# stored or printed f64 bits.
#
# WHAT THIS CHECKS (`tkc --check`, structured JSON):
#   1. E4031 (no `fix`) with exact expected/got for: an @i64 annotation on an
#      f64 map (via &f, a closure literal, a `let g=&f` value); a str
#      annotation on `.get(0)` of it; an @str annotation on a `let` copy;
#   2. clean: a matching @f64 / @bool annotation, `.len()` into i64, a closure
#      with no declared return (unmodelled), a fallible callee (unmodelled);
#   3. E4026 arity on `.map` unchanged (call passes 3 / 1).
#
# Negative control: main 58bd74e8 reports nothing for the cases in 1.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2304_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2304: diagnostics on a typed .map result (127.550)"
echo "---------------------------------------------------"

# check NAME BODY WANT -- WANT is "clean" or "CODE|expected|got" ("-" = any)
check() {
  local name="$1" body="$2" want="$3"
  printf 'm=p;\ni=io:std.io;\nt=$e{m:str};\nf=dbl(x:f64):f64{<x*2.0};\nf=big(x:f64):bool{<x>4.0};\nf=bad(x:f64):f64!$e{<x};\nf=main():i64{\n  let fs=@(1.5;2.5);\n  %s\n  <0\n};\n' "$body" > d.tk
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

check "@i64 annotation on &dbl map"   'let a:@i64=fs.map(&dbl);'                    'E4031|@(i64)|@(f64)'
check "@i64 annotation on closure map" 'let a:@i64=fs.map(fn(v:f64):f64{<v});'      'E4031|@(i64)|@(f64)'
check "@str annotation via let g=&dbl" 'let g=&dbl; let a:@str=fs.map(g);'          'E4031|@(str)|@(f64)'
check "str annotation on .get(0)"     'let a:str=fs.map(&dbl).get(0);'              'E4031|str|f64'
check "@str annotation on a let copy" 'let a=fs.map(&dbl); let b:@str=a;'           'E4031|@(str)|@(f64)'
check "@f64 annotation clean"         'let a:@f64=fs.map(&dbl);'                    'clean'
check "@bool annotation clean"        'let a:@bool=fs.map(&big);'                   'clean'
check ".len() into i64 clean"         'let a:i64=fs.map(&dbl).len();'               'clean'
check "undeclared closure return clean" 'let a:@i64=fs.map(fn(v:f64){<v});'         'clean'
check "fallible callee unmodelled"    'let a=fs.map(&bad);'                         'clean'
check "E4026 arity (3) unchanged"     'let a=fs.map(&dbl;1);'                       'E4026|-|-'
check "E4026 arity (1) unchanged"     'let a=fs.map();'                             'E4026|-|-'

echo ""
echo "C2304: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
