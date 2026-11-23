#!/usr/bin/env bash
# C2161_df_filter_str_op_val.sh -- df.filter reads its op and val as the strs
# its interface declares (story 138.66).
#
# THE DEFECT (integrate/wave-f-1123 5a65c2e9, and v3.2.0).  dataframe.tki
# declares df.filter(dataframe;str;str;str):dataframe!dferr and
# docs/stdlib/dataframe.md documents op as "<" "<=" "=" ">=" ">" "!=" and val
# as a number in a str.  tk_dataframe_filter_w passed op's pointer to
# df_filter as its int operator code and memcpy'd val's pointer bits into the
# f64 threshold, so every documented call -- df.filter(d;"a";">";"2") --
# kept no rows (the pointer is never an operator code 0..4, so no row matched)
# and a malformed op or val was never refused.
#
# THE FIX.  The glue maps op exactly to df_filter's code (df_filter gains
# "!=", code 5, the one documented op it lacked) and parses val with strtod in
# full.  Anything else is $err: an unknown op ("==", ""), a val that is empty
# or has trailing bytes ("2x", "abc"), an unknown or non-numeric column.
#
# WHAT IS ASSERTED: each of the six ops over a{1,2,3,2} with val "2" keeps
# exactly the rows the comparison selects (rows, and the CSV of the result,
# which also reads every cell of the kept rows); "2.5", "-1" and "1e0" parse;
# the error cases above are $err with the dferr message.
#
# Negative control: TKC=~/tk/tmpwork/wt_toke320/tkc (v3.2.0), or a tkc built
# at 5a65c2e9 -- every op case keeps 0 rows and no error case is $err.
#
# Story: 138.66
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2161_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2161: df.filter reads its str op and val (138.66)"
echo "--------------------------------------------------"

cat > filt.tk <<'EOF'
m=filt;
i=io:std.io;
i=df:std.dataframe;
f=show(d:$dataframe;col:$str;op:$str;val:$str):i64{
  let r=mt df.filter(d;col;op;val){$ok:x "\(df.shape(x).rows) \(df.tocsv(x))";$err:e "ERR \(e.msg)"};
  io.println("[\(col) \(op) \(val)] \(r)");
  <0
};
f=main():i64{
  let d=mt df.fromcsv("a,name\n1,x\n2,y\n3,z\n2,w\n"){$ok:x x;$err:e 0 as $dataframe};
  show(d;"a";"<";"2");
  show(d;"a";"<=";"2");
  show(d;"a";"=";"2");
  show(d;"a";">=";"2");
  show(d;"a";">";"2");
  show(d;"a";"!=";"2");
  show(d;"a";">";"2.5");
  show(d;"a";">";"-1");
  show(d;"a";"<=";"1e0");
  show(d;"a";"==";"2");
  show(d;"a";"";"2");
  show(d;"a";">";"");
  show(d;"a";">";"2x");
  show(d;"a";">";"abc");
  show(d;"name";">";"2");
  show(d;"nosuch";">";"2");
  <0
};
EOF

if ! "${TKC}" -o filt filt.tk > build.log 2>&1; then
    fail "filt.tk builds"
    sed 's/^/    /' build.log | head -20
    echo "C2161: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
pass "filt.tk builds"
./filt > raw.txt 2>&1
rc=$?
# df.tocsv ends its records with CRLF (RFC 4180); compare by line.
tr -d '\r' < raw.txt > out.txt
[ "${rc}" -eq 0 ] && pass "filt exits 0" || fail "filt exits 0 (got ${rc})"

MSG="ERR df.filter: the filter could not be applied (unknown or non-numeric column, unknown op, or a val that is not an f64)"
# The whole output, line for line: each kept row is its own line after the
# [col op val] line.
cat > want.txt <<EOF
[a < 2] 1 a,name
1,x
[a <= 2] 3 a,name
1,x
2,y
2,w
[a = 2] 2 a,name
2,y
2,w
[a >= 2] 3 a,name
2,y
3,z
2,w
[a > 2] 1 a,name
3,z
[a != 2] 2 a,name
1,x
3,z
[a > 2.5] 1 a,name
3,z
[a > -1] 4 a,name
1,x
2,y
3,z
2,w
[a <= 1e0] 1 a,name
1,x
[a == 2] ${MSG}
[a  2] ${MSG}
[a > ] ${MSG}
[a > 2x] ${MSG}
[a > abc] ${MSG}
[name > 2] ${MSG}
[nosuch > 2] ${MSG}
EOF
# Each case is checked on its own: its block runs from its [label] line to
# the next one.
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

echo "C2161: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
