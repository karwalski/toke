#!/usr/bin/env bash
# C2180_df_groupby_dataframe_array.sh -- df.groupby returns the [dataframe]
# its interface declares (story 127.518).
#
# THE DEFECT (integrate/wave-f-1123 fd81dd90, and v3.2.0).  dataframe.tki
# declares df.groupby(dataframe;str):[dataframe]!dferr and
# docs/stdlib/dataframe.md documents one sub-frame per distinct value of the
# str column.  tk_dataframe_groupby_w asked df_groupby for an aggregate over
# no column (always the empty result, so every call was $err) and, had it
# succeeded, would have returned a malloc'd C DfGroupResult* -- not a toke
# array, and its elements not registered dataframes.
#
# THE FIX.  New df_split (dataframe.c) builds one frame per distinct value
# (all columns, rows in their original order; groups in order of each value's
# first row); the glue returns them as a toke array, each frame registered
# (df_out, 138.63).  A frame with no rows gives @(); an unknown or f64 column
# is $err with the dferr message.
#
# WHAT IS ASSERTED: grouping r{e,w,e,n,w,e} gives 3 groups whose .len, and
# for each element df.shape and df.tocsv (every cell of every group), are
# exactly the expected sub-frames; a group is a dataframe that can be passed
# on (df.filter over a group; df.groupby of a group); a frame filtered to no
# rows gives 0 groups (@(), not $err); "v" (f64) and "nosuch" are $err.
#
# Negative control: TKC=~/tk/tmpwork/wt_toke320/tkc (v3.2.0) -- every call
# is $err, so no group line is printed.
#
# Story: 127.518
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2180_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2180: df.groupby returns [dataframe] (127.518)"
echo "-----------------------------------------------"

cat > grp.tk <<'EOF'
m=grp;
i=io:std.io;
i=df:std.dataframe;
f=show(tag:$str;d:$dataframe;col:$str):i64{
  let gs=mt df.groupby(d;col){$ok:g g;$err:e @()};
  let ok=mt df.groupby(d;col){$ok:g "ok";$err:e "ERR \(e.msg)"};
  io.println("[\(tag)] \(ok) len=\(gs.len)");
  lp(let i=0;i<gs.len;i=i+1){
    let g=gs.get(i);
    let s=df.shape(g);
    io.println("group \(i) \(s.rows)x\(s.cols)");
    io.println(df.tocsv(g));
  };
  <0
};
f=main():i64{
  let d=mt df.fromcsv("r,v,n\ne,1,a\nw,2,b\ne,3,c\nn,4,d\nw,5,e\ne,6,f\n"){$ok:x x;$err:e 0 as $dataframe};
  show("r";d;"r");
  let gs=mt df.groupby(d;"r"){$ok:g g;$err:e @()};
  let east=gs.get(0);
  let big=mt df.filter(east;"v";">";"2"){$ok:x x;$err:e 0 as $dataframe};
  io.println("[filter east v>2] \(df.shape(big).rows)");
  io.println(df.tocsv(big));
  show("regroup west";gs.get(1);"n");
  let empty=mt df.filter(d;"v";">";"100"){$ok:x x;$err:e 0 as $dataframe};
  show("empty";empty;"r");
  show("v";d;"v");
  show("nosuch";d;"nosuch");
  <0
};
EOF

if ! "${TKC}" -o grp grp.tk > build.log 2>&1; then
    fail "grp.tk builds"
    sed 's/^/    /' build.log | head -20
    echo "C2180: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
pass "grp.tk builds"
./grp > raw.txt 2>&1
rc=$?
# df.tocsv ends its records with CRLF (RFC 4180); compare by line.
tr -d '\r' < raw.txt > out.txt
[ "${rc}" -eq 0 ] && pass "grp exits 0" || fail "grp exits 0 (got ${rc})"

MSG="ERR df.groupby: the column could not be grouped"
cat > want.txt <<EOF
[r] ok len=3
group 0 3x3
r,v,n
e,1,a
e,3,c
e,6,f
group 1 2x3
r,v,n
w,2,b
w,5,e
group 2 1x3
r,v,n
n,4,d
[filter east v>2] 2
r,v,n
e,3,c
e,6,f
[regroup west] ok len=2
group 0 1x3
r,v,n
w,2,b
group 1 1x3
r,v,n
w,5,e
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

echo "C2180: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
