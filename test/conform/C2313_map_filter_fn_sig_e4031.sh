#!/usr/bin/env bash
# C2313_map_filter_fn_sig_e4031.sh — the function value handed to `.map` /
# `.filter` is judged against the receiver's element type (story 127.552).
#
# THE DEFECT: E4026 checked only the combinator's own arity, so
# `fs.map(&add)` (add:(f64;f64):f64, a second parameter nobody passes) and
# `@(1;2).map(&dbl)` (dbl:(f64):f64, an i64 element bitcast to a double)
# passed `tkc --check`, and the 127.548 adapter read an unpassed argument
# slot or reinterpreted the element's bits at run time.
#
# THE FIX: types.c check_combinator_fn_sig reads the value's signature as
# recv_map_fn_sig does and judges it at the call: map takes (T), filter
# takes (T) and returns bool or an integer.  E4031 on the argument, no `fix`.
#
# WHAT THIS CHECKS (`tkc --check`, structured JSON, schema 1.0):
#   1. E4031 with exact expected/got and no `fix` for: a two-parameter &f,
#      an f64 parameter over an @i64 (literal and named receiver), a
#      `let g=&f` value, a closure literal, a chained receiver, a filter
#      predicate returning str;
#   2. clean: matching &f and capturing-closure values, a str-returning map,
#      a bool and an i64 predicate, a closure with no declared return;
#   3. E4026 on the combinator's own arity unchanged.
#
# Negative control: 127.551 (26881357) reports nothing for the cases in 1.
#
# Story: 127.552
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2313_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2313: .map/.filter function value judged against the element type (127.552)"
echo "----------------------------------------------------------------------------"

# check NAME BODY WANT -- WANT is "clean" or "CODE|expected|got" ("-" = any)
check() {
  local name="$1" body="$2" want="$3"
  printf 'm=p;\ni=io:std.io;\nt=$e{m:str};\nf=add(a:f64;b:f64):f64{<a+b};\nf=addi(a:i64;b:i64):i64{<a+b};\nf=dbl(x:f64):f64{<x*2.0};\nf=inc(x:i64):i64{<x+1};\nf=big(x:f64):bool{<x>2.0};\nf=pos(x:i64):bool{<x>0};\nf=sh(x:i64):str{<\"s\"};\nf=cmp(a:i64;b:i64):i64{<a-b};\nf=cmpf(a:f64;b:f64):f64{<a-b};\nf=lt(a:i64;b:i64):bool{<a<b};\nf=cnt(a:i64;x:f64):i64{if(x>2.0){<a+1};<a};\nf=bad(a:f64;b:f64):f64!$e{<a};\nf=main():i64{\n  let fs=@(1.5;2.5);\n  let ns=@(1;2);\n  %s\n  <0\n};\n' "$body" > d.tk
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
parts = want.split("|"); code, exp, got = parts[0], parts[1], parts[2]
if len(recs) != 1: print("want one %s, got %s" % (code, [(r.get("error_code"), r.get("message")) for r in recs])); sys.exit()
r = recs[0]
bad = []
if r.get("error_code") != code: bad.append("code %s" % r.get("error_code"))
if exp != "-" and r.get("expected") != exp: bad.append("expected %r" % r.get("expected"))
if got != "-" and r.get("got") != got: bad.append("got %r" % r.get("got"))
if "fix" in r: bad.append("has fix %r" % r.get("fix"))
if len(parts) > 3 and parts[3] not in r.get("message", ""): bad.append("message %r" % r.get("message"))
if r.get("schema_version") != "1.0" or r.get("stage") != "type_check": bad.append("schema")
print("OK" if not bad else "; ".join(bad))
PY
)
  [ "$r" = OK ] && pass "$name" || fail "$name: $r"
}

check "map: two-parameter &add"         'let a=fs.map(&add);'                      'E4031|(f64):R|(f64;f64):f64|takes 2 parameters'
check "map: f64 param over @i64 literal"   'let a=@(1;2).map(&dbl);'                  'E4031|(i64):R|(f64):f64|parameter 1'
check "map: f64 param over @i64 binding"   'let a=ns.map(&dbl);'                      'E4031|(i64):R|(f64):f64|parameter 1'
check "map: let g=&dbl over @i64"          'let g=&dbl; let a=ns.map(g);'             'E4031|(i64):R|(f64):f64|parameter 1'
check "map: closure literal wrong param"   'let a=ns.map(fn(x:f64):f64{<x});'         'E4031|(i64):R|(f64):f64|parameter 1'
check "map: chained receiver"              'let a=fs.map(&dbl).map(&inc);'            'E4031|(f64):R|(i64):i64|parameter 1'
check "filter: f64 predicate over @i64"    'let a=ns.filter(&big);'                   'E4031|(i64):bool|(f64):bool|parameter 1'
check "filter: str-returning predicate"    'let a=ns.filter(&sh);'                    'E4031|(i64):bool|(i64):str|returns a'
check "filter: two-parameter predicate"    'let a=ns.filter(&lt);'                    'E4031|(i64):bool|(i64;i64):bool|takes 2 parameters'
check "map: matching &dbl clean"           'let a=fs.map(&dbl);'                      'clean'
check "map: matching closure clean"        'let k=2.0; let a=fs.map(fn(v:f64):f64{<v*k});' 'clean'
check "map: str-returning clean"           'let a=ns.map(&sh);'                       'clean'
check "filter: bool predicate clean"       'let a=fs.filter(&big);'                   'clean'
check "filter: i64 predicate clean"        'let a=ns.filter(&inc);'                   'clean'
check "map: closure, no declared return"   'let a=ns.map(fn(x:i64){<x+1});'           'clean'
check "E4026 arity (3) unchanged"          'let a=fs.map(&dbl;1);'                    'E4026|-|-'

echo ""
echo "C2313: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
