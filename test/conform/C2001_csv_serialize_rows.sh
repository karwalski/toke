#!/usr/bin/env bash
# C2001_csv_serialize_rows.sh -- csv.serialize takes the toke value its
# documentation names, @(@($str)), and returns the CSV text as a str
# (story 127.354, owner register 2.11 recommendation).
#
# THE DEFECT (integrate/wave-a-1122 0558545d, v3.2.0).  csv.tki declared
# csv.serialize(i64):i64 with "gen":"abi", and tk_csv_serialize_w read a
# packed C block, [nrows][StrArray...], that no toke value has.  A toke call
# with an array of rows compiled and segfaulted (v3.2.0: exit 139); an
# integer argument was accepted too (a str was refused, "expected an array").
#
# THE FIX.  csv.serialize(rows:[[str]]):str.  The glue walks the toke array
# of rows (runtime-abi.md §4) and writes what csv.writer(44) + csv.writerow
# per row + csv.flush writes: commas, RFC 4180 quoting, CRLF between rows, no
# trailing line break; no rows is "".  The result is a sealed str.
#
# WHAT IS ASSERTED.
#   1. a program serializes rows with a comma, a double-quote, a line break,
#      an empty field, an empty row and ragged rows: the bytes match exactly,
#      and match csv.writer + csv.writerow + csv.flush on the same rows;
#   2. no rows is "" (length 0), one row has no line break;
#   3. the result is a str: .len and interpolation work on it;
#   4. --check refuses a str and an i64 argument with E4031 naming
#      @(@(str)) (127.497: a [str] argument to a std [[str]] slot is not
#      judged yet -- not asserted here).
#
# Negative control: TKC=<v3.2.0 tkc> (ea7ddf22): 1 passed, 4 failed -- the
# program compiles and exits 139 with no output; csv.serialize(7) checks
# clean; csv.serialize("a,b") is E4031 "expected an array" with no
# @(@(str)) expectation.
# Measured on this branch: 7 passed.
#
# Story: 127.354
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2001_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2001: csv.serialize takes @(@(\$str)) and returns the CSV text (127.354)"
echo "----------------------------------------------------------------------"

cat > ser.tk <<'EOF'
m=ser;
i=csv:std.csv;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let rows=@(@("name";"note";"n");@("Bob, Jr.";"say \"hi\"";"");@();@("two
lines");@("a";"b";"c";"d"));
  let out=csv.serialize(rows);
  let w=csv.writer(44);
  lp(let i=0;i<rows.len;i=i+1){
    csv.writerow(w;rows.get(i))
  };
  let flushed=mt str.frombytes(csv.flush(w)) {$ok:s s;$err:e "<<bad utf-8>>"};
  io.print(out);
  io.print("|");
  io.println(if(out==flushed){"same"}el{"differs"});
  let none=csv.serialize(@());
  io.println("none=\(none.len)");
  let one=csv.serialize(@(@("x";"y")));
  io.println("one=[\(one)] len=\(one.len)");
  <0
};
EOF

if ! "${TKC}" ser.tk -o ser > cc.log 2>&1; then
    fail "the csv.serialize program compiles"
    sed 's/^/    /' cc.log | head -10
else
    pass "the csv.serialize program compiles"
    ./ser > out.bin 2> err.log
    rc=$?
    if [ "${rc}" -eq 0 ]; then pass "the program exits 0"; else fail "the program exits 0 (got ${rc})"; fi
    printf 'name,note,n\r\n"Bob, Jr.","say ""hi""",\r\n\r\n"two\nlines"\r\na,b,c,d|same\nnone=0\none=[x,y] len=3\n' > want.bin
    if cmp -s out.bin want.bin; then
        pass "the bytes are exact: quoting, empty field, empty row, ragged rows, CRLF, no trailing break"
        pass "csv.serialize equals csv.writer + csv.writerow + csv.flush on the same rows"
        pass "no rows is \"\" and one row has no line break; .len and interpolation work"
    else
        fail "the output bytes match"
        echo "    want:"; od -c want.bin | sed 's/^/      /' | head -8
        echo "    got:";  od -c out.bin  | sed 's/^/      /' | head -8
    fi
fi

for arg in '"a,b"' '7'; do
    printf 'm=bad;\ni=csv:std.csv;\nf=main():i64{\n  let s=csv.serialize(%s);\n  <0\n};\n' "${arg}" > bad.tk
    out="$("${TKC}" --check bad.tk 2>&1)"; rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031"' \
       && printf '%s' "${out}" | grep -q '"expected":"@(@(str))"'; then
        pass "--check refuses csv.serialize(${arg}) with E4031 expecting @(@(str))"
    else
        fail "--check refuses csv.serialize(${arg}) with E4031 expecting @(@(str)) (rc=${rc}: ${out:0:200})"
    fi
done

echo "C2001: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
