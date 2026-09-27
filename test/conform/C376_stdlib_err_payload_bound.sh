#!/usr/bin/env bash
# C376_stdlib_err_payload_bound.sh — a stdlib `T!E` failure binds its error
# record in the $err arm, and a later success in the same run takes the $ok
# arm (127.238, Batch GB2).
#
# THE DEFECT.  These wrappers returned a bare 0 on failure and were not on
# llvm.c's hand-kept slot-wrapper list, so the $err arm bound nil and
# `e.msg` stopped the program with RT005: auth (AuthErr), csv (csverr),
# dataframe (dferr), ml (mlerr), ws (wserr), template (tmplerr).  Their
# glues now store the record in the error slot and clear it on success;
# llvm.c derives the slot wrappers from the .tki (C375 keeps that exact).
# std.dataframe's exports are called as tk_dataframe_x_w, not the .tki's
# tk_df_x_w, which is why the derivation resolves both spellings.
#
# Negative control: TKC + stdlib from main at 9f901bf — cases 1, 3, 4, 5 red
# ("RT005: nil dereference"), case 2 red (build-failed: E4034 on `e.line`,
# the payload's type was never established).  Case 6 (a failure then a
# success: the ok arm is taken, the slot was cleared) is green on both.
#
# Story: 127.238

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C376_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
run() { printf '%s\n' "$1" > "${WORK}/p.tk"; rm -f "${WORK}/b"
        "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" >/dev/null 2>&1 || { echo build-failed; return; }
        "${WORK}/b" 2>&1 | tr '\n' '|'; }

expect "1-auth" "$(run 'm=main;i=io:std.io;i=auth:std.auth;
f=main():i64{
  io.println(mt auth.bearerextract("Basic xyz") {$ok:t "ok \(t)";$err:e "err \(e.msg) code=\(e.code)"});
  <0
};')" "err not a Bearer token code=0|"

expect "2-csv" "$(run 'm=main;i=io:std.io;i=csv:std.csv;i=str:std.str;
f=main():i64{
  io.println(mt csv.parse(str.bytes("")) {$ok:r "ok \(r.len)";$err:e "err \(e.msg) line=\(e.line)"});
  <0
};')" "err csv.parse: no rows (the input is empty or could not be parsed) line=0|"

expect "3-dataframe" "$(run 'm=main;i=io:std.io;i=df:std.dataframe;
f=main():i64{
  let frame=mt df.fromcsv("x,y\n1,2\n") {$ok:f f;$err:e <1};
  io.println(mt df.columnstr(frame;"nosuch") {$ok:v "ok";$err:e "err \(e.msg)"});
  <0
};')" "err df.columnstr: no such string column|"

expect "4-ws" "$(run 'm=main;i=io:std.io;i=ws:std.ws;
f=main():i64{
  io.println(mt ws.connect("ws://127.0.0.1:1/") {$ok:c "ok";$err:e "err \(e.msg)"});
  <0
};')" "err ws.connect: the connection could not be opened|"

expect "5-ml" "$(run 'm=main;i=io:std.io;i=ml:std.ml;
f=main():i64{
  io.println(mt ml.kmeanstrain(@();2;10) {$ok:m "ok";$err:e "err \(e.msg)"});
  <0
};')" "err ml.kmeanstrain: no model could be trained from these rows|"

# 6. Control: after a failure, a success reads as a success (the ok return
#    cleared the slot) — per module, in one run.
expect "6-cleared-after-failure" "$(run 'm=main;i=io:std.io;i=auth:std.auth;i=csv:std.csv;i=str:std.str;
f=main():i64{
  let a=mt auth.bearerextract("Basic xyz") {$ok:t "ok";$err:e "err"};
  let b=mt auth.bearerextract("Bearer tok1") {$ok:t "ok \(t)";$err:e "err"};
  let c=mt csv.parse(str.bytes("")) {$ok:r "ok";$err:e "err"};
  let d=mt csv.parse(str.bytes("a,b\n1,2\n")) {$ok:r "ok \(r.len)";$err:e "err"};
  io.println("\(a) \(b) \(c) \(d)");
  <0
};')" "err ok tok1 err ok 2|"

echo "C376: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
