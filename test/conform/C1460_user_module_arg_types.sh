#!/usr/bin/env bash
# C1460_user_module_arg_types.sh -- every argument of a call into another
# USER module is judged against the parameter type its generated .tki
# declares, by the rule a same-module call uses (story 127.378).
#
# THE DEFECT: check_tki_arg_types returns for any module that is not std.*,
# and 127.377's check_user_fn_args judged only FUNCTION-typed parameters, so
# `l.ap(dbl;"x")` (ap declared `(h:(i64):i64;x:i64)`) passed --check and
# printed the str's address doubled, where the same call inside one module
# is E4031 'expected i64, got str'.
#
# NOW: each argument whose parameter spelling the .tki reader types -- a
# scalar, [T], [K:V], an imported record, a function type -- is judged with
# types_compat / lit_narrows, E4031, no fix.  Every case is checked twice,
# imported (`l.f(..)`) and with the same functions declared in the module
# (`f(..)`); both must give the expected codes.  An interface whose
# parameter is the pre-127.376 "(" (case 22) stays unjudged, as before.
#
# 127.382: a SUM-typed parameter (`"params": ["sh"]`, the type export
# `"is_sum": true`) is judged too, as the same-module `s:$sh` is: an integer,
# a str, a record or another module-local type is E4031 (cases 21, 24-26); a
# variant literal of the sum and a value of it pass (case 27).  Before it,
# `l.psum(3)` checked rc 0 where `psum(3)` is E4031.
#
# Negative control: TKC built from fix/z166-imported-fn-types (4ef15404) --
# cases 1-14 red (imported rc 0); 15-23 green on both.  TKC built from
# fix/z169-user-args-getor-fn (87153202) -- cases 21, 24-26 red (imported
# rc 0), the rest green.
#
# Stories: 127.378, 127.382

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# The library's types and functions; also pasted into the same-module control.
TYPES='t=$rec{n:i64;s:str};
t=$sh{$ci:i64;$sq:str};'
FUNCS='f=dbl(x:i64):i64{<x*2};
f=ap(h:(i64):i64;x:i64):i64{<h(x)};
f=pu(x:u8):i64{<x as i64};
f=ps(s:str):i64{<s.len()};
f=pf(x:f64):f64{<x*2.0};
f=pb(b:bool):i64{<1};
f=pa(a:@(i64)):i64{<a.len()};
f=pn(a:@(@(i64))):i64{<a.len()};
f=pm(m:@(str:i64)):i64{<m.len()};
f=pr(r:$rec):i64{<r.n};
f=mk():$rec{<$rec{n:3;s:"x"}};
f=psum(s:$sh):i64{<1};
f=mks():$sh{<$sh{$sq:"q"}};'
printf 'm=c1460.lib;\n%s\n%s\n' "${TYPES}" "${FUNCS}" > "${WORK}/a_lib.tk"
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
[ -f "${WORK}/c1460.lib.tki" ] || echo "  FAIL: setup: --emit-interface produced no c1460.lib.tki"

codes() {   # codes FILE -- sorted error codes, then rc
    local out rc
    out="$(cd "${WORK}" && "${TKC}" --check "$1" 2>&1)"; rc=$?
    echo "$(printf '%s\n' "${out}" | grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' ')rc=${rc}"
}
# chk NAME BODY -- BODY spells the module prefix `L.`; prints "imported | same-module"
chk() {
    local imp same
    imp="$(printf '%s' "$2" | sed 's/L\./l./g')"; same="$(printf '%s' "$2" | sed 's/L\.//g')"
    printf 'm=c1460.main;\ni=io:std.io;\ni=l:c1460.lib;\nt=$loc{n:i64};\nf=tri(x:i64):i64{<x*3};\nf=main():i64{\n%s\n<0};\n' \
        "${imp}" > "${WORK}/$1.tk"
    printf 'm=c1460s;\ni=io:std.io;\nt=$loc{n:i64};\n%s\n%s\nf=tri(x:i64):i64{<x*3};\nf=main():i64{\n%s\n<0};\n' \
        "${TYPES}" "${FUNCS}" "${same}" > "${WORK}/$1s.tk"
    echo "$(codes "$1.tk") | $(codes "$1s.tk")"
}
BAD="E4031 rc=1 | E4031 rc=1"
OK="rc=0 | rc=0"

expect "1 str for an i64 (the reported call)"   "$(chk n1  'io.println("\(L.ap(tri;"x"))");')" "${BAD}"
expect "2 i64 for a str"                        "$(chk n2  'let a=L.ps(3);')" "${BAD}"
expect "3 f64 literal for an i64"               "$(chk n3  'let a=L.ap(tri;1.5);')" "${BAD}"
expect "4 literal out of u8 range"              "$(chk n4  'let a=L.pu(300);')" "${BAD}"
expect "5 i64 value for a u8"                   "$(chk n5  'let x:i64=3; let a=L.pu(x);')" "${BAD}"
expect "6 integer literal for an f64"           "$(chk n6  'let a=L.pf(3);')" "${BAD}"
expect "7 integer for a bool"                   "$(chk n7  'let a=L.pb(1);')" "${BAD}"
expect "8 @(str) for @(i64)"                    "$(chk n8  'let a=L.pa(@("a"));')" "${BAD}"
expect "9 @(i64) for @(@(i64))"                 "$(chk n9  'let a=L.pn(@(1));')" "${BAD}"
expect "10 wrong map key type"                  "$(chk n10 'let a=L.pm(@(1:1));')" "${BAD}"
expect "11 integer for a record"                "$(chk n11 'let a=L.pr(3);')" "${BAD}"
expect "12 a record of another name"            "$(chk n12 'let a=L.pr($loc{n:1});')" "${BAD}"
expect "13 a function for an i64"               "$(chk n13 'let a=L.ap(tri;tri);')" "${BAD}"
expect "14 str field of an imported record"     "$(chk n14 'let r=L.mk(); let a=L.ap(tri;r.s);')" "${BAD}"

expect "15 well-typed scalars (control)"        "$(chk p15 'let a=L.ap(tri;3); let b=L.ps("x"); let c=L.pf(3.0); let d=L.pb(true); let x:u8=3; let e=L.ap(tri;x);')" "${OK}"
expect "16 fitting literal for a u8 (control)"  "$(chk p16 'let a=L.pu(255);')" "${OK}"
expect "17 arrays and maps (control)"           "$(chk p17 'let a=L.pa(@(1;2)); let b=L.pn(@(@(1))); let c=L.pm(@("a":1)); let d=L.pa(@());')" "${OK}"
expect "18 imported record (control)"           "$(chk p18 'let r=L.mk(); let a=L.pr(r); let b=L.pr(L.mk()); let c=L.ps(r.s);')" "${OK}"
expect "19 function value (control)"            "$(chk p19 'let a=L.ap(tri;5); let b=L.ap(fn(x:i64):i64{<x};5);')" "${OK}"

# 20: the reported call no longer builds; the well-typed one runs as in one module
cat > "${WORK}/r20.tk" <<'EOF'
m=c1460.r;
i=io:std.io;
i=l:c1460.lib;
f=tri(x:i64):i64{<x*3};
f=main():i64{io.println("\(l.ap(tri;14)) \(l.ps("abc")) \(l.pr(l.mk()))");<0};
EOF
rm -f "${WORK}/r20.bin"
( cd "${WORK}" && tk_build "${TKC}" a_lib.tk r20.tk -o r20.bin >/dev/null 2>&1 ) || tk_build_failed $?
r20="$([ -x "${WORK}/r20.bin" ] && tk_run "${WORK}/r20.bin" 2>&1 | tr '\n' ' ')"
expect "20 well-typed imported calls run" "${r20}" "42 3 3 "

# 21: a sum-typed parameter is judged by the same-module rule (127.382)
expect "21 integer for a sum (127.382)"         "$(chk n21 'let a=L.psum(3);')" "${BAD}"

# 22: an interface written before 127.376 spelled a function type "("
mkdir -p "${WORK}/old"
sed 's/"([^"]*):[^"]*"/"("/g' "${WORK}/c1460.lib.tki" > "${WORK}/old/c1460.lib.tki"
printf 'm=c1460.o;\ni=l:c1460.lib;\nf=main():i64{let a=l.ap(1;2);<0};\n' > "${WORK}/old/o22.tk"
o22="$(cd "${WORK}/old" && "${TKC}" --check o22.tk >/dev/null 2>&1; echo "rc=$?")"
expect "22 old '(' spelling: unjudged, i64 argument still fine" \
    "$(grep -c '"):' "${WORK}/old/c1460.lib.tki") ${o22}" "0 rc=0"

# 23: E4031 here carries no fix (the declared type comes from an interface;
# which repair was meant is not certain)
f23="$(cd "${WORK}" && "${TKC}" --check n6.tk 2>&1 | grep -c '"fix"')"
expect "23 no fix field on the imported E4031" "${f23}" "0"

# 24-27: more sum-typed parameters (127.382)
expect "24 str for a sum"                       "$(chk n24 'let a=L.psum("x");')" "${BAD}"
expect "25 a local record for a sum"            "$(chk n25 'let a=L.psum($loc{n:1});')" "${BAD}"
expect "26 an imported record for a sum"        "$(chk n26 'let r=L.mk(); let a=L.psum(r);')" "${BAD}"
expect "27 variant literals and a sum value (control)" \
    "$(chk p27 'let a=L.psum($sh{$ci:1}); let b=L.psum($sh{$sq:"x"}); let s=L.mks(); let c=L.psum(s); let d=L.psum(L.mks());')" "${OK}"

echo "C1460: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
