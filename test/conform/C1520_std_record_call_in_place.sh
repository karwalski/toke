#!/usr/bin/env bash
# C1520_std_record_call_in_place.sh -- a std call whose .tki return is a
# FIELDED record with no error type (`time.toparts(0)` -> timeparts,
# `math.linreg(..)` -> linregresult, `svg.style(..)`, `time.duration(..)`)
# is typed as that record wherever it stands, not only when bound first
# (story 127.386).
#
# THE DEFECT: call_decl_ret_type gave the `let` binding the std record
# (127.66), but the in-place call kept no type -- 127.384 adopted the record
# in place for USER modules only -- so nothing judged it: `time.toparts(0)
# .nosuch`, `time.toparts(0)+1`, `qs(time.toparts(0))` (str parameter),
# `$loc{s:time.toparts(0)}`, `"\(time.toparts(0))"` all passed --check, and
# `j.enc(time.toparts(t))` printed the record's address where the bound
# form prints the JSON object.  Bound first, each is E4025 / E4031 / E4032.
#
# NOW: every check case is run twice, in place and bound by a `let` first;
# both must give the expected codes.  Field-less std handles (strbuf,
# svgelem, vechandle, securebuf) are adopted in place too since 127.392
# (C1570); case 14 is red on 6d270505.
#
# Negative control: TKC built from fix/z172-imported-record-sum-lit
# (8f261a6f) -- cases 1-10 and 16 red; 11-15, 17, 18 green.
#
# Story: 127.386

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
RUN="${REPO_ROOT}/test/run_test.sh"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

H='m=c1520;
i=io:std.io;
i=time:std.time;
i=math:std.math;
i=svg:std.svg;
i=s:std.str;
i=j:std.json;
t=$loc{s:str};
f=qs(s:str):i64{<1};
f=qi(x:i64):i64{<x};'

codes() {
    local out rc
    out="$(cd "${WORK}" && "${TKC}" --check "$1" 2>&1)"; rc=$?
    echo "$(printf '%s\n' "${out}" | grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' ')rc=${rc}"
}
# chk NAME CALL USE -- USE contains `X`; checked once with X = CALL in place
# and once with `let r=CALL;` first and X = r.  Prints "in-place | bound".
chk() {
    local inp bnd
    inp="$(printf '%s' "$3" | sed "s|X|$2|g")"; bnd="let r=$2; $(printf '%s' "$3" | sed 's|X|r|g')"
    printf '%s\nf=main():i64{\n%s\n<0};\n' "${H}" "${inp}" > "${WORK}/$1.tk"
    printf '%s\nf=main():i64{\n%s\n<0};\n' "${H}" "${bnd}" > "${WORK}/$1b.tk"
    echo "$(codes "$1.tk") | $(codes "$1b.tk")"
}
TP='time.toparts(0)'
LR='math.linreg(@(1.0;2.0);@(1.0;2.0))'
B31="E4031 rc=1 | E4031 rc=1"
B25="E4025 rc=1 | E4025 rc=1"
OK="rc=0 | rc=0"

expect "1 unknown field in place (the reported case)" "$(chk n1 "${TP}" 'let a=X.nosuch;')" "${B25}"
expect "2 in arithmetic (the reported case)"    "$(chk n2  "${TP}" 'let a=X+1;')" "${B31}"
expect "3 as a str argument"                    "$(chk n3  "${TP}" 'let a=qs(X);')" "${B31}"
expect "4 as an i64 argument"                   "$(chk n4  "${TP}" 'let a=qi(X);')" "${B31}"
expect "5 as a str field"                       "$(chk n5  "${TP}" 'let a=$loc{s:X};')" "${B31}"
expect "6 as an array element beside a str"     "$(chk n6  "${TP}" 'let a=@("x";X);')" "${B31}"
expect "7 interpolated"                         "$(chk n7  "${TP}" 'io.println("\(X)");')" "E4032 rc=1 | E4032 rc=1"
expect "8 linreg unknown field"                 "$(chk n8  "${LR}" 'let a=X.nosuch;')" "${B25}"
expect "9 linreg in arithmetic"                 "$(chk n9  "${LR}" 'let a=X*2;')" "${B31}"
expect "10 svg.style / time.duration unknown field" \
    "$(chk n10a 'svg.style("red";"blue";1.0)' 'let a=X.nosuch;') ; $(chk n10b 'time.duration(0;1000)' 'let a=X.nosuch;')" \
    "${B25} ; ${B25}"
expect "11 annotated str let (already judged)"  "$(chk n11 "${TP}" 'let a:str=X;')" "${B31}"
expect "12 well-typed in place (control)" \
    "$(chk p12 "${TP}" 'let a=X.year+1; let b=qi(X.month); let c=time.fromparts(X);')" "${OK}"
expect "13 linreg field read (control)"         "$(chk p13 "${LR}" 'let a=X.slope*2.0;')" "${OK}"
expect "14 field-less std handle in place (127.392: now judged too)" \
    "$(chk p14 's.buf()' 'let a=X+1;')" "${B31}"

# 15/16: well-typed in-place calls run as the bound form does
cat > "${WORK}/r15.tk" <<'EOF'
m=c1520r;
i=io:std.io;
i=time:std.time;
i=math:std.math;
f=yr(p:$timeparts):i64{<p.year};
f=main():i64{
let w=@(time.toparts(3600000);time.toparts(7200000));
io.println("\(time.toparts(90061000).day) \(time.toparts(90061000).hour+1) \(yr(time.toparts(0))) \(time.fromparts(time.toparts(5000))) \(w.get(1).hour) \(math.linreg(@(1.0;2.0;3.0);@(2.0;4.0;6.0)).slope)");
<0};
EOF
rm -f "${WORK}/r15.bin"
( cd "${WORK}" && "${RUN}" 60 "${TKC}" r15.tk -o r15.bin >/dev/null 2>&1 )
r15="$([ -x "${WORK}/r15.bin" ] && "${RUN}" 20 "${WORK}/r15.bin" 2>&1 | tr '\n' ' ')"
expect "15 well-typed in-place calls run" "${r15}" "2 2 1970 5000 2 2 "

cat > "${WORK}/r16.tk" <<'EOF'
m=c1520j;
i=io:std.io;
i=time:std.time;
i=j:std.json;
f=main():i64{
let p=time.toparts(90061000);
io.println(j.enc(p));
io.println(j.enc(time.toparts(90061000)));
<0};
EOF
rm -f "${WORK}/r16.bin"
( cd "${WORK}" && "${RUN}" 60 "${TKC}" r16.tk -o r16.bin >/dev/null 2>&1 )
r16="$([ -x "${WORK}/r16.bin" ] && "${RUN}" 20 "${WORK}/r16.bin" 2>&1 | tr '\n' ' ')"
J='{"year":1970,"month":1,"day":2,"hour":1,"min":1,"sec":1}'
expect "16 j.enc of an in-place record encodes it as bound" "${r16}" "${J} ${J} "

# 17: no fix field on these diagnostics
f17="$(cd "${WORK}" && "${TKC}" --check n3.tk 2>&1 | grep -c '"fix"')"
expect "17 no fix field" "${f17}" "0"

# 18: a user-module record in place is still typed (127.384 unchanged)
printf 'm=c1520.lib;\nt=$rec{n:i64};\nf=mk():$rec{<$rec{n:3}};\n' > "${WORK}/a_lib.tk"
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
printf 'm=c1520.main;\ni=l:c1520.lib;\nf=main():i64{let a=l.mk()+1;<0};\n' > "${WORK}/u18.tk"
expect "18 user-module record in place (unchanged)" "$(codes u18.tk)" "E4031 rc=1"

echo "C1520: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
