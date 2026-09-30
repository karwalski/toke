#!/usr/bin/env bash
# C411_stdlib_err_slot_bare_glues.sh — the `T!E` glues 127.238 left on the
# bare-glue list for encoding, infer, mlx, ocr, pdf, process, time, toon.dec
# and xlsx now bind their error payload, and a later success in the same run
# takes the $ok arm (127.241).
#
# THE DEFECT.  These glues returned a bare 0/null on failure and never wrote
# @tk_current_error, so they stayed on g_err_slot_bare[] (C375) and the $err
# arm bound nil: `e.msg` was RT005 for a record error, and a match on a sum
# error's variants did not build (E9005).  Worse, several took the WRONG arm:
# encoding.b64decode/hexdecode of invalid input returned an empty array
# through $ok; infer.generate and mlx.generate returned a placeholder string
# through $ok; time.parse of the epoch returned 0 and so took $err.
#
# Cases (one per module, a real failure binding the payload):
#   1 encoding.b64decode  e.msg          5 time.parse       $parseerr
#   2 encoding.b64urldecode (new wrapper) 6 toon.dec        $parse
#   3 infer.generate      e.msg, e.code  7 pdf.open         $baddoc
#   4 mlx.embed (new wrapper) e.msg      8 xlsx.open        $badworkbook
#   9 process.spawn       $notfound     10 ocr.text         $badimage / $noengine
#  11 time.parse of the epoch is ok 0 (a zero value is not a failure)
#  12 cleared — per module, a success after a failure takes $ok
#
# NOT COVERED: args.get and env.get.  Their glue fills the slot now too, but
# `m.get(x)` parses as an index expression, and the $err arm's payload is
# derived only for a call-expression scrutinee (llvm.c
# classify_fallible_call), so `e.msg` on args.get is still RT005 — reported
# to the orchestrator as a compiler gap, not asserted here.
#
# Negative control: TKC + stdlib from main at 8efba9ea (C411_STDLIB) — 0
# passed, 12 failed: 1 took $ok ("ok len=0"), 3 took $ok ("ok [infer: not
# available]"), 2 and 4 build-failed (no wrapper: quarantined), 6-10
# build-failed (E9005), 5 printed "rangeerr||" (a 2-arm match on the nil
# payload was lowered as ok/err: a silent wrong arm, not an error), 11 took
# $err for the epoch, 12 read hexdecode("zz") as $ok.
#
# Story: 127.241

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C411_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
run() { printf '%s\n' "$1" > "${WORK}/p.tk"; rm -f "${WORK}/b"
        "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" >"${WORK}/build.log" 2>&1 || { echo build-failed; return; }
        (cd "${WORK}" && ./b --allow-all 2>&1) | tr '\n' '|'; }

expect "1-encoding-b64decode" "$(run 'm=main;i=io:std.io;i=enc:std.encoding;i=s:std.str;
f=main():i64{
  io.println(mt enc.b64decode("!!not base64") {$ok:v s.concat("ok len=";s.fromint(v.len));$err:e s.concat("err ";e.msg)});
  <0
};')" "err encoding.b64decode: not base64 (a character outside the standard alphabet)|"

expect "2-encoding-b64urldecode" "$(run 'm=main;i=io:std.io;i=enc:std.encoding;i=s:std.str;
f=main():i64{
  io.println(mt enc.b64urldecode("aGVsbG8_-w") {$ok:v s.concat("ok len=";s.fromint(v.len));$err:e s.concat("err ";e.msg)});
  io.println(mt enc.b64urldecode("a+b/") {$ok:v "ok";$err:e s.concat("err ";e.msg)});
  <0
};')" "ok len=7|err encoding.b64urldecode: not base64url (a character outside the URL-safe alphabet)|"

expect "3-infer-generate" "$(run 'm=main;i=io:std.io;i=infer:std.infer;i=s:std.str;
f=main():i64{
  io.println(mt infer.generate(0;"hi";16) {$ok:v s.concat("ok ";v);$err:e s.concat(s.concat(e.msg;" code=");s.fromint(e.code as i64))});
  <0
};')" "infer.generate: local inference is not linked into this build (std.infer's glue is a stub) code=0|"

expect "4-mlx-embed" "$(run 'm=main;i=io:std.io;i=mlx:std.mlx;i=s:std.str;
f=main():i64{
  io.println(mt mlx.embed(0;"hi") {$ok:v "ok";$err:e s.concat("err ";e.msg)});
  <0
};')" "err mlx.embed: MLX is not available (std.mlx's glue is a stub)|"

expect "5-time-parse-parseerr" "$(run 'm=main;i=io:std.io;i=time:std.time;i=s:std.str;
f=main():i64{
  io.println(mt time.parse("not a date";"%Y-%m-%d") {$ok:v "ok";$err:e mt e {$parseerr:m s.concat("parseerr|";m);$rangeerr:m s.concat("rangeerr|";m)}});
  <0
};')" "parseerr|strptime failed: input does not match format|"

expect "6-toon-dec-parse" "$(run 'm=main;i=io:std.io;i=toon:std.toon;i=s:std.str;
f=main():i64{
  io.println(mt toon.dec("") {$ok:v "ok";$err:e mt e {$parse:m s.concat("parse|";m);$type:m s.concat("type|";m);$missing:m s.concat("missing|";m)}});
  <0
};')" "parse|empty input|"

expect "7-pdf-open-baddoc" "$(run 'm=main;i=io:std.io;i=pdf:std.pdf;i=s:std.str;
f=main():i64{
  io.println(mt pdf.open(s.bytes("not a pdf")) {$ok:v "ok";$err:e mt e {$baddoc:m s.concat("baddoc|";m);$encrypted:m m;$nopage:m m;$toolarge:m m;$unsupported:m m;$io:m m}});
  <0
};')" "baddoc|pdf: no %PDF- header in the first 1024 bytes|"

expect "8-xlsx-open-badworkbook" "$(run 'm=main;i=io:std.io;i=xlsx:std.xlsx;i=s:std.str;
f=main():i64{
  io.println(mt xlsx.open(s.bytes("not a workbook")) {$ok:v "ok";$err:e mt e {$badworkbook:m s.concat("badworkbook|";m);$badpart:m m;$nosheet:m m;$toolarge:m m;$io:m m}});
  io.println(mt xlsx.openfile("nosuch.xlsx") {$ok:v "ok";$err:e mt e {$badworkbook:m m;$badpart:m m;$nosheet:m m;$toolarge:m m;$io:m s.concat("io|";m)}});
  <0
};')" "badworkbook|zip: not a zip archive, or its central directory is corrupt|io|zip: cannot open archive file|"

expect "9-process-spawn-notfound" "$(run 'm=main;i=io:std.io;i=proc:std.process;i=s:std.str;
f=main():i64{
  io.println(mt proc.spawn(@("/nonexistent/c411-binary")) {$ok:v "ok";$err:e mt e {$notfound:m s.concat("notfound|";m);$permission:m s.concat("permission|";m);$io:m s.concat("io|";m)}});
  <0
};')" "notfound|executable not found|"

# 10. ocr: the variant depends on the host — Vision refuses the bytes as
#     $badimage; a host with no recogniser says $noengine.  Either is a bound
#     payload; nil (RT005) or a build failure is the defect.
OCR="$(run 'm=main;i=io:std.io;i=ocr:std.ocr;i=s:std.str;
f=main():i64{
  io.println(mt ocr.text(s.bytes("not an image")) {$ok:v "ok";$err:e mt e {$noengine:m "noengine";$badimage:m "badimage";$toosmall:m "toosmall";$nolanguage:m "nolanguage";$failed:m "failed";$io:m "io"}});
  <0
};')"
if [ "$(uname -s)" = "Darwin" ]; then expect "10-ocr-text-variant" "${OCR}" "badimage|"
else expect "10-ocr-text-variant" "${OCR}" "noengine|"; fi

expect "11-time-parse-epoch-is-ok" "$(run 'm=main;i=io:std.io;i=time:std.time;i=s:std.str;
f=main():i64{
  io.println(mt time.parse("1970-01-01";"%Y-%m-%d") {$ok:v s.concat("ok ";s.fromint(v as i64));$err:e "err"});
  <0
};')" "ok 0|"

expect "12-cleared-after-failure" "$(run 'm=main;i=io:std.io;i=enc:std.encoding;i=time:std.time;i=toon:std.toon;i=pdf:std.pdf;i=proc:std.process;i=s:std.str;
f=main():i64{
  let a=mt enc.hexdecode("zz") {$ok:v "ok";$err:e "err"};
  let b=mt enc.hexdecode("4142") {$ok:v s.frombytes(v);$err:e "err"};
  let c=mt time.parse("x";"%Y") {$ok:v "ok";$err:e "err"};
  let d=mt time.parse("2000";"%Y") {$ok:v "ok";$err:e "err"};
  let t1=mt toon.dec("") {$ok:v "ok";$err:e "err"};
  let t2=mt toon.dec("data[1]{id}:\n1\n") {$ok:v "ok";$err:e "err"};
  let p1=mt proc.spawn(@("/nonexistent/c411-binary")) {$ok:v "ok";$err:e "err"};
  let p2=mt proc.spawn(@("true")) {$ok:v "ok";$err:e "err"};
  io.println(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(a;" ");b);" ");c);" ");d);" ");t1);" ");t2);" ");p1);" ");p2));
  <0
};')" "err AB err ok err ok err ok|"

echo "C411: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
