#!/usr/bin/env bash
# C2190_df_columnstr_str_array.sh -- df.columnstr returns the [str] its
# interface declares (story 127.520).
#
# THE DEFECT (integrate/wave-f-1123 81836ed9, and v3.2.0).  dataframe.tki
# declares df.columnstr(dataframe;str):[str]!dferr.  tk_dataframe_columnstr_w
# returned df_columnstr's malloc'd char** unchanged -- no toke array header --
# so `.len` of the ok value read the word before the malloc block (0 for a
# 2-row column) and `.get(i)` was bounds-checked against that word.
#
# THE FIX.  The glue copies the column's values into a tk_arr_alloc block of
# sealed strings, one per row in row order.  A str column of a frame with no
# rows is @(); an unknown column or an f64 column is $err with the dferr
# message.
#
# WHAT IS ASSERTED: for a 3-row and a 2-row frame, .len and every element
# (with its str.len) of the str column; the column of a group (df.groupby,
# 127.518) and of a filtered frame; a frame filtered to no rows gives len 0
# (@(), not $err); "v" (f64) and "nosuch" are $err.
#
# Negative control: TKC=~/tk/tmpwork/wt_toke320/tkc (v3.2.0) -- len reads 0
# for every ok column, so no element line is printed.
#
# Story: 127.520
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2190_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2190: df.columnstr returns [str] (127.520)"
echo "-------------------------------------------"

cat > cs.tk <<'EOF'
m=cs;
i=io:std.io;
i=s:std.str;
i=df:std.dataframe;
f=show(tag:$str;d:$dataframe;col:$str):i64{
  let xs=mt df.columnstr(d;col){$ok:v v;$err:e @()};
  let ok=mt df.columnstr(d;col){$ok:v "ok";$err:e "ERR \(e.msg)"};
  io.println("[\(tag)] \(ok) len=\(xs.len)");
  lp(let i=0;i<xs.len;i=i+1){
    let x=xs.get(i);
    io.println("\(i) <\(x)> \(s.len(x))");
  };
  <0
};
f=main():i64{
  let d=mt df.fromcsv("r,v,n\neast,1,alpha\nwest,2,b\neast,3,\n"){$ok:x x;$err:e 0 as $dataframe};
  show("r";d;"r");
  show("n";d;"n");
  let d2=mt df.fromcsv("name,score\nada,9\ngrace,7\n"){$ok:x x;$err:e 0 as $dataframe};
  show("two rows";d2;"name");
  let gs=mt df.groupby(d;"r"){$ok:g g;$err:e @()};
  show("group east n";gs.get(0);"n");
  let big=mt df.filter(d;"v";">";"1"){$ok:x x;$err:e 0 as $dataframe};
  show("filter v>1 r";big;"r");
  let empty=mt df.filter(d;"v";">";"100"){$ok:x x;$err:e 0 as $dataframe};
  show("empty";empty;"r");
  show("v";d;"v");
  show("nosuch";d;"nosuch");
  <0
};
EOF

if ! "${TKC}" -o cs cs.tk > build.log 2>&1; then
    fail "cs.tk builds"
    sed 's/^/    /' build.log | head -20
    echo "C2190: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
pass "cs.tk builds"
./cs > out.txt 2>&1
rc=$?
[ "${rc}" -eq 0 ] && pass "cs exits 0" || fail "cs exits 0 (got ${rc})"

MSG="ERR df.columnstr: no such string column"
cat > want.txt <<EOF
[r] ok len=3
0 <east> 4
1 <west> 4
2 <east> 4
[n] ok len=3
0 <alpha> 5
1 <b> 1
2 <> 0
[two rows] ok len=2
0 <ada> 3
1 <grace> 5
[group east n] ok len=2
0 <alpha> 5
1 <> 0
[filter v>1 r] ok len=2
0 <west> 4
1 <east> 4
[empty] ok len=0
[v] ${MSG} len=0
[nosuch] ${MSG} len=0
EOF
split_cases() { awk -v p="$1" '/^\[/{n++} {print > (p "_" sprintf("%02d", n) ".txt")}' "$2"; }
split_cases w want.txt
split_cases g out.txt
for wf in w_*.txt; do
    gf="g_${wf#w_}"
    label="$(head -1 "${wf}" | sed 's/\].*/]/')"
    if [ -f "${gf}" ] && cmp -s "${wf}" "${gf}"; then
        pass "${label} $(head -1 "${wf}" | sed 's/^\[[^]]*\] //' | cut -c1-50)"
    else
        fail "${label}: want '$(tr '\n' '|' < "${wf}")' got '$( [ -f "${gf}" ] && tr '\n' '|' < "${gf}")'"
    fi
done
cmp -s want.txt out.txt && pass "the whole output matches" || fail "the whole output matches"

echo "C2190: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
