#!/usr/bin/env bash
# C384_db_row_cols.sh — std.db's $dbrow reads its `cols` field, and the row.*
# accessors read a $dbrow literal the same way as a queried one (story 135.30).
#
# THE DEFECT.  tkc reads `r.cols` as slot 0 of db.tki's Row {cols:[[str]]}.
# db.one/db.many handed back the C Row {col_names; col_values; col_nulls;
# col_count}, so `r.cols` was the col_names pointer read as an array
# (`.len` gave 0, `.get(0)` an RT003 bounds error).  The row.* accessors
# read their argument as a C Row, so the `$dbrow{cols:@()}` of db.md's own
# error arms was read past its one slot.
#
# THE FIX (src/stdlib/db.h, db_glue.c).  A $dbrow (the $row of 135.30; renamed 127.251) is a TkRowRec {cols}: one
# @(name; value) pair per column, value "" for SQL NULL.  db.one/db.many
# build it; row.str/i64/u64/f64/bool, db.getfield and db.print read it
# (row_view).  Marked tki-record; make check-tki-layout checks it (C389).
#
# WHAT IS ASSERTED (a scratch SQLite file in the test's own directory):
#   1. db.one: .cols.len, each pair's name and value, NULL as ""
#   2. row.str / row.i64 on the queried row
#   3. db.many: two rows, each reading its own cols
#   4. a $dbrow literal: row.str finds its column, row.i64 parses it
#   5. NEGATIVE CONTROL: a missing column is $err on both kinds of row, and
#      the empty literal of db.md's error arms is $err, not a crash
#
# C384_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf: 0/6 -- the first `r.cols.get` stops the program with RT003
# (index 0 out of bounds for length 0).
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C384_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c384_XXXXXX")"
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
echo "C384: std.db \$dbrow.cols and row.* on literals (135.30)"
echo "------------------------------------------------------------"
cat > p.tk <<'TK'
m=p;
i=io:std.io;
i=str:std.str;
i=db:std.db;
f=pr(p:@(str)):str{ <str.concat(p.get(0); str.concat("="; p.get(1))) };
f=main():i64{
  let h=mt db.open("c384.db") {$ok:v v;$err:e 0};
  let x=db.exec("CREATE TABLE t(a TEXT, b INTEGER, c TEXT)";@());
  let y=db.exec("INSERT INTO t VALUES(?, ?, NULL)";@("hello";"42"));
  let z=db.exec("INSERT INTO t VALUES(?, ?, ?)";@("bye";"7";"x"));
  let r=mt db.one("SELECT a, b, c FROM t WHERE b=42";@()) {$ok:v v;$err:e $dbrow{cols:@()}};
  io.println(str.concat(str.fromint(r.cols.len as i64); str.concat(" "; str.concat(pr(r.cols.get(0)); str.concat(" "; str.concat(pr(r.cols.get(1)); str.concat(" "; pr(r.cols.get(2)))))))));
  io.println(str.concat(mt row.str(r;"a") {$ok:v v;$err:e "err"}; str.concat(" "; str.fromint(mt row.i64(r;"b") {$ok:v v;$err:e -1}))));
  let rs=mt db.many("SELECT a, b FROM t ORDER BY b";@()) {$ok:v v;$err:e @()};
  io.println(str.concat(str.fromint(rs.len as i64); str.concat(" "; str.concat(pr(rs.get(0).cols.get(0)); str.concat(" "; pr(rs.get(1).cols.get(1)))))));
  let lit=$dbrow{cols:@(@("k";"v");@("n";"12"))};
  io.println(str.concat(mt row.str(lit;"k") {$ok:v v;$err:e "err"}; str.concat(" "; str.fromint(mt row.i64(lit;"n") {$ok:v v;$err:e -1}))));
  io.println(str.concat(mt row.str(r;"nope") {$ok:v v;$err:e "err"}; str.concat(" "; str.concat(mt row.str(lit;"nope") {$ok:v v;$err:e "err"}; str.concat(" "; mt row.str($dbrow{cols:@()};"a") {$ok:v v;$err:e "err"})))));
  <0
};
TK
got="$(run p)"
f() { printf '%s' "${got% rc=*}" | cut -d'|' -f"$1"; }
expect "1  db.one: .cols (NULL as \"\")" "$(f 1)" "3 a=hello b=42 c="
expect "2  row.str / row.i64 on the queried row" "$(f 2)" "hello 42"
expect "3  db.many: each row reads its own cols" "$(f 3)" "2 a=bye b=42"
expect "4  a \$dbrow literal through row.str / row.i64" "$(f 4)" "v 12"
expect "5  NEGATIVE CONTROL: a missing column, and the empty literal, are \$err" "$(f 5)" "err err err"
expect "   exit status" "${got##* }" "rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
