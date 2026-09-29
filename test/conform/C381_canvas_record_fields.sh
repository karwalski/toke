#!/usr/bin/env bash
# C381_canvas_record_fields.sh — every field of std.canvas's $canvas and
# $canvasop reads what its name says (story 135.30).
#
# THE DEFECT.  tkc reads `c.f` as 8-byte slot <index of f in canvas.tki>:
# $canvas is {id; width:u32; height:u32; ops:[canvasop]}.  The glue handed
# back the C TkCanvas {id; uint32 width; uint32 height; op_count; JS buffer
# ...}: width and height shared slot 1, so `c.width` was 300|200<<32 =
# 858993459500, `c.height` read the op count, and `c.ops` read the JS
# buffer as an array.
#
# THE FIX (src/stdlib/canvas.h, canvas.c, tk_array.h).  TkCanvas starts
# with the four record fields, each a whole slot, and ops is a real toke
# array of $canvasop {tag; data} kept by every drawing call (tag: the Canvas
# 2D member the op ends with; data: its JS statement).  Marked
# tki-record-prefix / tki-record; make check-tki-layout checks both (C389).
#
# WHAT IS ASSERTED:
#   1. canvas.new: .id, .width, .height, and .ops empty
#   2. after fillrect + filltext: .ops has two ops, tags and data in order,
#      and the chained result is the same canvas (fields unchanged)
#   3. NEGATIVE CONTROL: a second canvas of another size reads its own
#      width/height and its own (empty) ops, so no field is a constant
#   4. canvas.tohtml still carries the width/height and both statements
#
# C381_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf all 7 fail: the program dies with SIGSEGV on the first
# `c.ops.len` (slot 3 is the JS buffer's NULL pointer); with the ops reads
# removed it prints width 858993459500 and height 0.
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C381_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c381_XXXXXX")"
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
echo "C381: std.canvas record fields (135.30)"
echo "------------------------------------------------------------"
cat > p.tk <<'TK'
m=p;
i=io:std.io;
i=str:std.str;
i=canvas:std.canvas;
f=show(c:$canvas):str{
  <str.concat(c.id; str.concat(" "; str.concat(str.fromint(c.width as i64);
     str.concat("x"; str.concat(str.fromint(c.height as i64);
     str.concat(" ops="; str.fromint(c.ops.len as i64)))))))
};
f=main():i64{
  let c=canvas.new("cv";300;200);
  io.println(show(c));
  let d=canvas.fillrect(c;1.0;2.0;3.0;4.0;"red");
  let e=canvas.filltext(d;"hi";5.0;6.0;"10px serif";"blue");
  io.println(show(e));
  io.println(e.ops.get(0).tag);
  io.println(e.ops.get(0).data);
  io.println(e.ops.get(1).tag);
  io.println(e.ops.get(1).data);
  let g=canvas.new("other";64;32);
  io.println(show(g));
  let h=canvas.tohtml(e);
  io.println(str.fromint(str.len(h) as i64));
  io.println(h);
  <0
};
TK
got="$(run p)"
f() { printf '%s' "${got}" | cut -d'|' -f"$1"; }
expect "1  canvas.new: id, width, height, empty ops" "$(f 1)" "cv 300x200 ops=0"
expect "2a after fillrect+filltext: two ops, same canvas" "$(f 2)" "cv 300x200 ops=2"
expect "2b op 0" "$(f 3)|$(f 4)" "fillRect|_c.fillStyle='red'; _c.fillRect(1,2,3,4);"
expect "2c op 1" "$(f 5)|$(f 6)" "fillText|_c.font='10px serif'; _c.fillStyle='blue'; _c.fillText('hi',5,6);"
expect "3  NEGATIVE CONTROL: another canvas reads its own fields" "$(f 7)" "other 64x32 ops=0"
expect "4  tohtml carries width, height and both statements" \
    "$(printf '%s' "${got}" | grep -c 'width="300" height="200".*fillRect(1,2,3,4).*fillText(.hi.,5,6)')" "1"
expect "   exit status" "${got##* }" "rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
