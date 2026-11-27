#!/usr/bin/env bash
# C2314_reduce_fold_sort_fn_sig_e4031.sh — the function value handed to
# `.reduce` / `.fold` / `.sort` is judged against the receiver's element type
# and, for a reduction, the accumulator (story 127.552).
#
# THE DEFECT: `ns.reduce(0.0;&add)` (add:(f64;f64):f64 over an @i64),
# `fs.sort(&cmp)` (cmp:(i64;i64):i64 over an @f64), `ns.sort(&inc)` (one
# parameter) and `fs.reduce(1;&add)` (an i64 seed in an f64 accumulator,
# read as the double 5e-324) passed `tkc --check`.
#
# THE FIX: types.c check_combinator_fn_sig: reduce / fold take (A;T):A, the
# seed must be an A (types_compat or a fitting literal, as a direct call
# judges it) and the return must flow back into A; sort takes (T;T) and
# returns a number or a bool (tk_arr_sort reads the comparator's sign; both
# are accepted today and stay accepted).  E4031 on the argument (on the seed
# for a seed that is not an A), no `fix`.
#
# WHAT THIS CHECKS (`tkc --check`, structured JSON, schema 1.0):
#   1. E4031 with exact expected/got and no `fix` for each shape above, a
#      fold through a closure, a reduce whose return is not its accumulator,
#      a fallible accumulator function, and a str-returning comparator;
#   2. clean: matching reduce / fold (&f, closure, mixed (i64;f64):i64 over
#      @f64), an i64 and a bool comparator over @i64, an f64 comparator over
#      @f64, an untyped closure;
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2314_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2314: .reduce/.fold/.sort function value judged (127.552)"
echo "----------------------------------------------------------"

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

check "reduce: f64 fn over @i64"          'let a=ns.reduce(0.0;&add);'               'E4031|(f64;i64):f64|(f64;f64):f64|parameter 2'
check "reduce: i64 seed, f64 accumulator"  'let a=fs.reduce(1;&add);'                 'E4031|f64|i64|initial value'
check "fold: i64 fn over @f64 (closure)"   'let a=fs.fold(0;fn(a:i64;b:i64):i64{<a+b});' 'E4031|(i64;f64):i64|(i64;i64):i64|parameter 2'
check "reduce: one-parameter fn"           'let a=ns.reduce(0;&inc);'                 'E4031|(A;i64):A|(i64):i64|takes 1 parameter'
check "reduce: return is not accumulator"  'let a=ns.fold(0;fn(a:i64;b:i64):str{<"s"});' 'E4031|(i64;i64):i64|(i64;i64):str|returns a'
check "reduce: fallible fn"                'let a=fs.reduce(0.0;&bad);'               'E4031|(f64;f64):f64|(f64;f64):f64!$e|returns a'
check "sort: i64 comparator over @f64"     'let a=fs.sort(&cmp);'                     'E4031|(f64;f64):i64|(i64;i64):i64|parameter 1'
check "sort: one-parameter comparator"     'let a=ns.sort(&inc);'                     'E4031|(i64;i64):i64|(i64):i64|takes 1 parameter'
check "sort: str-returning comparator"     'let a=ns.sort(fn(a:i64;b:i64):str{<"s"});' 'E4031|(i64;i64):i64|(i64;i64):str|returns a'
check "reduce: matching f64 clean"         'let a=fs.reduce(0.0;&add);'               'clean'
check "fold: matching i64 clean"           'let a=ns.fold(0;&addi);'                  'clean'
check "reduce: mixed accumulator clean"    'let a=fs.reduce(0;&cnt);'                 'clean'
check "reduce: closure clean"              'let a=fs.reduce(0.0;fn(a:f64;b:f64):f64{<a+b});' 'clean'
check "sort: i64 comparator clean"         'let a=ns.sort(&cmp);'                     'clean'
check "sort: bool comparator clean"        'let a=ns.sort(&lt);'                      'clean'
check "sort: f64 comparator clean"         'let a=fs.sort(&cmpf);'                    'clean'
check "reduce: closure, no declared return" 'let a=ns.reduce(0;fn(a:i64;b:i64){<a+b});' 'clean'
check "E4026 arity (1) unchanged"          'let a=fs.reduce(0.0);'                    'E4026|-|-'

echo ""
echo "C2314: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
