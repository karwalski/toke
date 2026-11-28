#!/usr/bin/env bash
# C1481_imported_sum_typed.sh -- a SUM type another USER module exports is
# typed in the importer as a same-module sum is: its variant literals
# (`$sh{$ci:1}`), its values returned from that module (`l.mks()`), and the
# name written as a type (`s:$sh`, `let x:$sh`, `:$sh`, a field `$sh`)
# (story 127.385).
#
# THE DEFECT: imported_struct_type answers NULL for a sum, so in the
# importer `$sh{$ci:1}` and `l.mks()` were TY_UNKNOWN and never judged:
# `let b:str=$sh{$ci:1}` and `l.pr($sh{$ci:1})` (pr declared (r:$rec))
# passed --check and printed 0; a parameter `s:$sh` returned as a str
# passed; `mt l.mks() {$ci:v v}` drew no E4010.  In one module each is
# E4031 / E4032 / E4025 / E4010.
#
# NOW: those are the TY_STRUCT 127.382 built for a sum parameter -- named
# for the sum, its variants as fields -- so every position judges them,
# including E4010 variant exhaustiveness (the .tki variant names carry the
# `$`, as a local sum's do).  Every case is checked imported and in one
# module; both must give the expected codes.  A std sum is not affected.
#
# Negative control: TKC built from fix/z171-sum-args-getor-leak (183fb159)
# -- the E4031/E4032/E4025/E4010 cases red (imported rc 0); controls green.
#
# Story: 127.385, 127.559

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

TYPES='t=$rec{n:i64;s:str};
t=$sh{$ci:i64;$sq:str};'
FUNCS='f=ps(s:str):i64{<s.len()};
f=pr(r:$rec):i64{<r.n};
f=psum(s:$sh):i64{<1};
f=mk():$rec{<$rec{n:3;s:"xy"}};
f=mks():$sh{<$sh{$sq:"qq"}};
f=mki():$sh{<$sh{$ci:42}};
f=rs(s:$sh):i64{<mt s{$ci:v v;$sq:t t.len()}};'
printf 'm=c1481.lib;\n%s\n%s\n' "${TYPES}" "${FUNCS}" > "${WORK}/a_lib.tk"
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
[ -f "${WORK}/c1481.lib.tki" ] || echo "  FAIL: setup: --emit-interface produced no c1481.lib.tki"

codes() {
    local out rc
    out="$(cd "${WORK}" && "${TKC}" --check "$1" 2>&1)"; rc=$?
    echo "$(printf '%s\n' "${out}" | grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' ')rc=${rc}"
}
# chk NAME TOPLEVEL BODY -- `L.` is the module prefix; prints "imported | same-module"
chk() {
    local imp same top tops
    imp="$(printf '%s' "$3" | sed 's/L\./l./g')"; same="$(printf '%s' "$3" | sed 's/L\.//g')"
    top="$(printf '%s' "$2" | sed 's/L\./l./g')"; tops="$(printf '%s' "$2" | sed 's/L\.//g')"
    printf 'm=c1481.main;\ni=io:std.io;\ni=l:c1481.lib;\nt=$loc{s:str};\nt=$w{s:$sh};\nf=qs(s:str):i64{<1};\n%s\nf=main():i64{\n%s\n<0};\n' \
        "${top}" "${imp}" > "${WORK}/$1.tk"
    printf 'm=c1481s;\ni=io:std.io;\n%s\nt=$loc{s:str};\nt=$w{s:$sh};\n%s\nf=qs(s:str):i64{<1};\n%s\nf=main():i64{\n%s\n<0};\n' \
        "${TYPES}" "${FUNCS}" "${tops}" "${same}" > "${WORK}/$1s.tk"
    echo "$(codes "$1.tk") | $(codes "$1s.tk")"
}
B31="E4031 rc=1 | E4031 rc=1"
B10="E4010 rc=1 | E4010 rc=1"
OK="rc=0 | rc=0"

# variant literal
expect "1 literal for an annotated str let (reported)" "$(chk n1 '' 'let b:str=$sh{$ci:1};')" "${B31}"
expect "2 literal for a record parameter (reported)"   "$(chk n2 '' 'let a=L.pr($sh{$ci:1});')" "${B31}"
expect "3 literal for a str argument"          "$(chk n3  '' 'let a=L.ps($sh{$ci:1});')" "${B31}"
expect "4 literal to a local function"         "$(chk n4  '' 'let a=qs($sh{$ci:1});')" "${B31}"
expect "5 literal returned as a str"           "$(chk n5  'f=g():str{<$sh{$ci:1}};' 'let a=g();')" "${B31}"
expect "6 literal as a str field"              "$(chk n6  '' 'let a=$loc{s:$sh{$ci:1}};')" "${B31}"
expect "7 literal bound, then for a str"       "$(chk n7  '' 'let r=$sh{$ci:1}; let a:str=r;')" "${B31}"
expect "8 literal interpolated"                "$(chk n8  '' 'io.println("\($sh{$ci:1})");')" "E4032 rc=1 | E4032 rc=1"
expect "9 literal in arithmetic"               "$(chk n9  '' 'let a=$sh{$ci:1}+1;')" "${B31}"
expect "10 literal beside a str in an array"   "$(chk n10 '' 'let a=@("x";$sh{$ci:1});')" "${B31}"
expect "11 field read on a literal"            "$(chk n11 '' 'let a=$sh{$ci:1}.nosuch;')" "E4025 rc=1 | E4025 rc=1"
expect "12 match on a literal missing a variant" "$(chk n12 '' 'let a=mt $sh{$ci:1} {$ci:v v};')" "${B10}"
# a sum returned from the other module, in place and bound
expect "13 returned sum for a str, in place"   "$(chk n13 '' 'let a:str=L.mks();')" "${B31}"
expect "14 returned sum for a record param"    "$(chk n14 '' 'let a=L.pr(L.mks());')" "${B31}"
expect "15 returned sum bound, then for a str" "$(chk n15 '' 'let r=L.mks(); let a=L.ps(r);')" "${B31}"
expect "16 match on a returned sum missing a variant" "$(chk n16 '' 'let a=mt L.mks() {$ci:v v};')" "${B10}"
expect "17 bound returned sum, match missing a variant" "$(chk n17 '' 'let r=L.mks(); let a=mt r {$sq:t t.len()};')" "${B10}"
# the type name written in the importer
expect "18 a \$sh parameter returned as a str"  "$(chk n18 'f=h(s:$sh):str{<s};' 'let a=0;')" "${B31}"
expect "19 a \$sh parameter matched missing a variant" "$(chk n19 'f=h(s:$sh):i64{<mt s{$ci:v v}};' 'let a=0;')" "${B10}"
expect "20 a str returned as \$sh"              "$(chk n20 'f=h():$sh{<"x"};' 'let a=0;')" "${B31}"
expect "21 a record returned as \$sh"           "$(chk n21 'f=h():$sh{<L.mk()};' 'let a=0;')" "${B31}"
expect "22 a \$sh field read as a str"          "$(chk n22 'f=h(w:$w):str{<w.s};' 'let a=0;')" "${B31}"
expect "23 annotated \$sh let returned as str"  "$(chk n23 'f=h():str{let x:$sh=L.mks(); <x};' 'let a=0;')" "${B31}"
# controls
expect "24 well-typed uses (control)" \
    "$(chk p24 'f=h(s:$sh):i64{<mt s{$ci:v v;$sq:t t.len()}};
f=g():$sh{<$sh{$ci:2}};
f=k():$w{<$w{s:L.mks()}};' 'let a=L.psum($sh{$ci:1}); let b=L.psum(L.mks()); let c=h(L.mki()); let d=L.rs(g()); let e=mt L.mks() {$ci:v v;$sq:t t.len()}; let r=$sh{$sq:"x"}; let f=mt r {$ci:v v;$sq:t t.len()}; let x:$sh=L.mks(); let y=@($sh{$ci:1};L.mks());')" "${OK}"

# 25: well-typed uses run as in one module
cat > "${WORK}/r25.tk" <<'EOF'
m=c1481.r;
i=io:std.io;
i=l:c1481.lib;
t=$w{s:$sh;n:i64};
f=loc(s:$sh):i64{<mt s{$ci:v v+1;$sq:t t.len()+1}};
f=e(x:i64):i64!$sh{if(x>0){<x};<$sh{$sq:"neg"}};
f=main():i64{
let a=mt l.mks(){$ci:v v;$sq:t t.len()};
let b=mt $sh{$sq:"hello"}{$ci:v v;$sq:t t.len()};
let w=$w{s:l.mki();n:1};
let q=mt e(0-1){$ok:v v;$err:x l.rs(x)};
io.println("\(a) \(b) \(l.rs($sh{$ci:5})) \(loc(l.mks())) \(l.rs(w.s)) \(q)");
<0};
EOF
rm -f "${WORK}/r25.bin"
( cd "${WORK}" && tk_build "${TKC}" a_lib.tk r25.tk -o r25.bin >/dev/null 2>&1 ) || tk_build_failed $?
r25="$([ -x "${WORK}/r25.bin" ] && tk_run "${WORK}/r25.bin" 2>&1 | tr '\n' ' ')"
expect "25 well-typed imported sums run" "${r25}" "2 5 5 3 42 3 "

# 26: no fix field on these E4031s
f26="$(cd "${WORK}" && "${TKC}" --check n1.tk 2>&1 | grep -c '"fix"')"
expect "26 no fix field" "${f26}" "0"

echo "C1481: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
