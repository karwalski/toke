#!/usr/bin/env bash
# C386_ml_record_fields.sh — std.ml's records read what their names say, and
# ml.linregfit fits the documented model (story 135.30).
#
# THE DEFECT.  tkc reads `x.f` as 8-byte slot <index of f in ml.tki>.
# $linearmodel is {coef:[f64]; intercept:f64}; ml.linregfit returned a bare
# {slope; intercept} pair of doubles, so `m.coef` read the slope's bits as
# an array handle (SIGSEGV).  Its wrapper also read the documented
# @($row) samples as doubles and fitted one slope, and ml.linregpredict
# read its @(f64) features handle as a single double.
#
# THE FIX (ml.h, ml.c, ml_glue.c).  ml.linregfit takes one $row per sample
# and fits one coefficient per feature (ml_linregfit_multi: least squares,
# normal equations with partial pivoting; $err when singular or short),
# returning a TkLinearModelRec; ml.linregpredict reads coef . x +
# intercept from the record, so a literal model predicts too.  The k-means
# and tree records, already built slot for slot, are now marked structs;
# make check-tki-layout checks all five (C389).
#
# WHAT IS ASSERTED:
#   1. ml.linregfit on ml.md's two-feature samples: coef.len 2, coef 1 and
#      2, and linregpredict at (10, 1) is 12
#   2. linregpredict of a $linearmodel literal
#   3. $err (not a crash) for identical samples and for too few targets
#   4. ml.kmeanstrain: .k, .centroids[i].id, .centroids[i].center
#   5. ml.dtreefit: .maxdepth, and ml.dtreepredict still answers
#   6. NEGATIVE CONTROL: a second fit on other data reads other coef
#
# C386_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf: 0/8 -- the program dies with SIGSEGV on `m.coef`.
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C386_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c386_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# run NAME: build NAME.tk and run it; prints its stdout lines joined by '|',
# then " rc=<exit status>" (a crash shows as rc=139, a build failure as
# "compile failed: <first error>").
run() {
    if ! "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1; then
        echo "compile failed: $(grep -m1 '^error' "$1.log")"; return
    fi
    local out rc
    out=$(./"$1" 2>"$1.err"); rc=$?
    echo "$(printf '%s' "${out}" | tr '\n' '|') rc=${rc}"
}
echo "C386: std.ml record fields and ml.linregfit (135.30)"
echo "------------------------------------------------------------"
cat > p.tk <<'TK'
m=p;
i=io:std.io;
i=str:std.str;
i=ml:std.ml;
f=coefs(m:$linearmodel):str{
  let out=mut.str.fromint(m.coef.len as i64);
  lp(let j=0;j<m.coef.len;j=j+1){
    let c:f64=m.coef.get(j);
    out=str.concat(out; str.concat(" "; str.fromf64(c)));
  };
  <out
};
f=main():i64{
  let feats=@($row{vals:@(1.0;2.0)};$row{vals:@(2.0;3.0)};$row{vals:@(3.0;5.0)};$row{vals:@(4.0;4.0)});
  let m=mt ml.linregfit(feats;@(5.0;8.0;13.0;12.0)) {$ok:v v;$err:e $linearmodel{coef:@();intercept:0.0}};
  io.println(coefs(m));
  io.println(str.fromf64(ml.linregpredict(m;@(10.0;1.0))));
  io.println(str.fromf64(ml.linregpredict($linearmodel{coef:@(2.0);intercept:1.0};@(3.0))));
  let same=@($row{vals:@(1.0)};$row{vals:@(1.0)};$row{vals:@(1.0)});
  let s=mt ml.linregfit(same;@(1.0;2.0;3.0)) {$ok:v "ok";$err:e "err"};
  let t=mt ml.linregfit(feats;@(1.0)) {$ok:v "ok";$err:e "err"};
  io.println(str.concat(s; str.concat(" "; t)));
  let rows=@($row{vals:@(1.0;1.0)};$row{vals:@(5.0;5.0)};$row{vals:@(1.5;1.5)};$row{vals:@(5.5;5.2)});
  let km=mt ml.kmeanstrain(rows;2;100) {$ok:v v;$err:e $kmeansmodel{centroids:@();k:0}};
  let cc:f64=km.centroids.get(1).center.get(0);
  io.println(str.concat(str.fromint(km.k as i64); str.concat(" "; str.concat(str.fromint(km.centroids.get(1).id as i64); str.concat(" "; str.fromf64(cc))))));
  let dt=mt ml.dtreefit(rows;@("a";"b";"a";"b");3) {$ok:v v;$err:e $dtreemodel{nodes:0;maxdepth:0;arena:0}};
  io.println(str.concat(str.fromint(dt.maxdepth as i64); str.concat(" "; ml.dtreepredict(dt;@(5.1;5.1)))));
  let o=mt ml.linregfit(@($row{vals:@(1.0)};$row{vals:@(2.0)};$row{vals:@(3.0)});@(3.0;5.0;7.0)) {$ok:v v;$err:e $linearmodel{coef:@();intercept:0.0}};
  io.println(str.concat(coefs(o); str.concat(" b="; str.fromf64(o.intercept))));
  <0
};
TK
got="$(run p)"
f() { printf '%s' "${got% rc=*}" | cut -d'|' -f"$1"; }
expect "1a ml.linregfit: one coef per feature" "$(f 1)" "2 1 2"
expect "1b linregpredict at (10, 1)" "$(f 2)" "12"
expect "2  linregpredict of a \$linearmodel literal" "$(f 3)" "7"
expect "3  \$err for identical samples and for too few targets" "$(f 4)" "err err"
expect "4  ml.kmeanstrain: k, centroids[1].id, centroids[1].center[0]" "$(f 5)" "2 1 5.25"
expect "5  ml.dtreefit: maxdepth, and dtreepredict" "$(f 6)" "3 b"
expect "6  NEGATIVE CONTROL: another fit reads its own coef and intercept" "$(f 7)" "1 2 b=1"
expect "   exit status" "${got##* }" "rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
