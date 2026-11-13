#!/usr/bin/env bash
# C1690_db_many_row_cols_typed.sh -- the `$ok` binding of an `mt` over a std
# call returning an array of std records (`db.many`'s `[dbrow]!dberr`) is
# typed as that array, so a nested read of a row's fields lowers by the
# record's .tki layout (story 127.424).
#
# WHAT WAS WRONG (base 586185a5, and toke main):
#   `let rs=mt db.many(..) {$ok:v v;$err:e @()}; let r0=rs.get(0);
#   io.println("\(r0.cols.get(0).get(1))")` printed an address: the
#   checker's mt_ok_payload_type adopted a std record (`dbrow!dberr`) and an
#   array of str/bool/f64 (127.297 / 127.333), but not an array of std
#   records, so `rs` was bound untyped and `cols` / `.get` read as i64.  The
#   same read on a db.one row, or with `let rs:@($dbrow)=..`, printed the
#   value.  The 127.335 field rule (`kmeansmodel.centroids` "[centroid]")
#   already typed such arrays inside a record; mt_ok_payload_type now shares
#   it (std_record_array_type).
#
# WHAT IS ASSERTED
#   1. run: db.many / db.manyon rows unannotated -- `r0.cols.get(0).get(1)`,
#      the inline `rs.get(0).cols.get(1).get(1)`, `.len` of a value, and a
#      `lp` over the rows -- print the values (under TK_STR_VERIFY=1)
#   2. run: csv.parse's `[csvrow]` -- `rows.get(1).fields.get(1)` (a control:
#      it already printed the value on the base; it must keep doing so)
#   3. run: `mt rows.get(1) {$ok:r r;$err:e $csvrow{..}}` -- a result match
#      over an imported record is not a sum match (no false E4010)
#   4. --check: the bound value now carries the record, so a misspelt field
#      `rs.get(0).colz` is E4025 and an `$err` arm of another array type
#      (`@(1;2)`) is E4011 (both clean on the base); `$err:e @()` and
#      `$err:e 0` stay clean
#
# Negative control: C1690_TKC=<path> runs another compiler (its own stdlib
# dir is used).  Measured: base 586185a5 tkc 3 passed, 3 failed (1 printed
# addresses; the E4025 and E4011 cases were clean); this branch 6 passed.
#
# Story: 127.424
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1690_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1690_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1690: mt over [stdrecord]!E binds the typed array (127.424)"
echo "------------------------------------------------------------"

run_tk() {  # $1 name, $2 expected stdout; program in $1.tk
    if ! "${TKC}" --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1; then
        fail "$1: compile"; head -c 400 "$1.log" | sed 's/^/        /'; echo; return
    fi
    got="$(TK_STR_VERIFY=1 "./$1" 2>&1)"
    if [ "${got}" = "$2" ]; then pass "$1: ${2//$'\n'/ | }"
    else fail "$1: got '${got//$'\n'/ | }', want '${2//$'\n'/ | }'"; fi
}
codes() {  # $1 file: the sorted error codes --check reports
    "${TKC}" --check "$1" 2>&1 | grep -o '"error_code":"E[0-9]*"' | cut -d'"' -f4 | sort -u | tr '\n' ' '
}

cat > many.tk <<'TOKE'
m=many;
i=io:std.io;
i=db:std.db;
f=main():i64{
  let h=mt db.open("many.db") {$ok:v v;$err:e 0};
  let x=db.exec("CREATE TABLE t(a TEXT, b TEXT)";@());
  let y=db.exec("INSERT INTO t VALUES(?, ?)";@("tok";"en"));
  let z=db.exec("INSERT INTO t VALUES(?, ?)";@("ooke";"fr"));
  let rs=mt db.many("SELECT a, b FROM t ORDER BY a DESC";@()) {$ok:v v;$err:e @()};
  let r0=rs.get(0);
  io.println("r0 \(r0.cols.get(0).get(1)) \(r0.cols.get(0).get(1).len)");
  io.println("inline \(rs.get(1).cols.get(1).get(1))");
  let ro=mt db.manyon(h;"SELECT a FROM t ORDER BY a";@()) {$ok:v v;$err:e 0};
  lp(let k=0;k<ro.len;k=k+1){io.println("on \(ro.get(k).cols.get(0).get(1))")};
  <0
};
TOKE
run_tk many "r0 tok 3
inline fr
on ooke
on tok"

cat > csvp.tk <<'TOKE'
m=csvp;
i=io:std.io;
i=str:std.str;
i=csv:std.csv;
f=main():i64{
  let rows=mt csv.parse(str.bytes("a,b\nc,dd\n")) {$ok:v v;$err:e @()};
  io.println("\(rows.len) \(rows.get(1).fields.get(1)) \(rows.get(1).fields.get(1).len)");
  <0
};
TOKE
run_tk csvp "2 dd 2"

# A result match over an element of the typed array: the element is an
# imported record (a product), so its fields are not variants -- this drew a
# false E4010 "missing variant(s) 'fields'" (docs/stdlib/csv.md block 7)
# once rows was typed, and on the base with `let rows:@($csvrow)=..`.
cat > elem.tk <<'TOKE'
m=elem;
i=io:std.io;
i=str:std.str;
i=csv:std.csv;
f=main():i64{
  let rows=mt csv.parse(str.bytes("a,b\nc,dd\n")) {$ok:v v;$err:e @()};
  let hdr=mt rows.get(1) {$ok:r r;$err:e $csvrow{fields:@()}};
  io.println("\(hdr.fields.len) \(hdr.fields.get(0))");
  <0
};
TOKE
run_tk elem "2 c"

chk() {  # $1 name, $2 the mt line, $3 the expected codes ("" = clean)
    printf 'm=%s;\ni=io:std.io;\ni=db:std.db;\nf=main():i64{\n  %s\n  <0\n};\n' "$1" "$2" > "$1.tk"
    got="$(codes "$1.tk")"
    if [ "${got}" = "$3" ]; then pass "--check $1: '${got}'"
    else fail "--check $1: got '${got}', want '$3'"; fi
}
chk fld   'let rs=mt db.many("x";@()) {$ok:v v;$err:e @()}; let c=rs.get(0).colz;' "E4025 "
chk arm   'let rs=mt db.many("x";@()) {$ok:v v;$err:e @(1;2)};' "E4011 "
chk clean 'let a=mt db.many("x";@()) {$ok:v v;$err:e @()}; let b=mt db.many("x";@()) {$ok:v v;$err:e 0}; io.println("\(a.len) \(b.len)");' ""

echo "------------------------------------------------------------"
echo "C1690: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
