#!/usr/bin/env bash
# C1480_imported_record_call_in_place.sh -- a call into another USER module
# whose .tki return is a record (`l.mk()`, "return": "rec") is typed as that
# record wherever it stands, not only when bound first (story 127.384).
#
# THE DEFECT: call_decl_ret_type adopted the imported record for a `let`
# binding (127.66), but the call expression's own type stayed unknown, so
# used IN PLACE nothing judged it: `io.println("\(l.pi(l.mk()))")` (pi
# declared i64) passed --check and printed the record's address doubled;
# `let a:str=l.mk()`, `<l.mk()` from a str function, `$loc{s:l.mk()}`,
# `l.mk()+1`, `@("x";l.mk())` and `l.mk().nosuch` all passed.  Bound first,
# or with the same functions declared in one module, each is E4031 / E4032
# / E4025 / E4080.
#
# NOW: every case is checked twice, imported (`l.f(..)`) and with the same
# declarations in one module (`f(..)`); both must give the expected codes.
# (A fielded std record return is typed in place too since 127.386, C1520.)
#
# Negative control: TKC built from fix/z171-sum-args-getor-leak (183fb159)
# -- cases 1-11 and 13 red (imported rc 0); 12, 14-16 green.
#
# Story: 127.384, 127.559

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
FUNCS='f=pi(x:i64):i64{<x*2};
f=ps(s:str):i64{<s.len()};
f=pr(r:$rec):i64{<r.n};
f=psum(s:$sh):i64{<1};
f=mk():$rec{<$rec{n:3;s:"xy"}};'
printf 'm=c1480.lib;\n%s\n%s\n' "${TYPES}" "${FUNCS}" > "${WORK}/a_lib.tk"
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
[ -f "${WORK}/c1480.lib.tki" ] || echo "  FAIL: setup: --emit-interface produced no c1480.lib.tki"

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
    printf 'm=c1480.main;\ni=io:std.io;\ni=l:c1480.lib;\nt=$loc{s:str};\nf=qs(s:str):i64{<1};\n%s\nf=main():i64{\n%s\n<0};\n' \
        "${top}" "${imp}" > "${WORK}/$1.tk"
    printf 'm=c1480s;\ni=io:std.io;\n%s\nt=$loc{s:str};\n%s\nf=qs(s:str):i64{<1};\n%s\nf=main():i64{\n%s\n<0};\n' \
        "${TYPES}" "${FUNCS}" "${tops}" "${same}" > "${WORK}/$1s.tk"
    echo "$(codes "$1.tk") | $(codes "$1s.tk")"
}
B31="E4031 rc=1 | E4031 rc=1"
OK="rc=0 | rc=0"

expect "1 in place as an i64 argument (the reported call)" "$(chk n1 '' 'io.println("\(L.pi(L.mk()))");')" "${B31}"
expect "2 in place for an annotated str let"   "$(chk n2  '' 'let a:str=L.mk();')" "${B31}"
expect "3 in place as a str argument"          "$(chk n3  '' 'let a=L.ps(L.mk());')" "${B31}"
expect "4 in place to a local function"        "$(chk n4  '' 'let a=qs(L.mk());')" "${B31}"
expect "5 in place as a returned str"          "$(chk n5  'f=g():str{<L.mk()};' 'let a=g();')" "${B31}"
expect "6 in place as a str field"             "$(chk n6  '' 'let a=$loc{s:L.mk()};')" "${B31}"
expect "7 in place in arithmetic"              "$(chk n7  '' 'let a=L.mk()+1;')" "${B31}"
expect "8 in place as an array element"        "$(chk n8  '' 'let a=@("x";L.mk());')" "${B31}"
expect "9 in place for a sum parameter"        "$(chk n9  '' 'let a=L.psum(L.mk());')" "${B31}"
expect "10 in place interpolated"              "$(chk n10 '' 'io.println("\(L.mk())");')" "E4032 rc=1 | E4032 rc=1"
expect "11 unknown field read in place"        "$(chk n11 '' 'let a=L.mk().nosuch;')" "E4025 rc=1 | E4025 rc=1"
expect "12 bound first (unchanged)"            "$(chk n12 '' 'let r=L.mk(); let a:str=r; let b=L.pi(r);')" "${B31}"
expect "13 variant match on it in place" \
    "$(chk n13 '' 'let a=mt L.mk() {$ci:v v;$sq:t t.len()};')" "E4080 rc=1 | E4080 rc=1"
expect "14 well-typed in place (control)" \
    "$(chk p14 '' 'let a=L.pr(L.mk()); let b=L.ps(L.mk().s); let c=L.mk().n+1; let d=$loc{s:L.mk().s}; io.println("\(L.mk().n)");')" "${OK}"

# 15: well-typed in-place uses run as in one module
cat > "${WORK}/r15.tk" <<'EOF'
m=c1480.r;
i=io:std.io;
i=l:c1480.lib;
f=main():i64{let w=@(l.mk();l.mk());io.println("\(l.pr(l.mk())) \(l.mk().s) \(l.mk().s.len()) \(l.pi(l.mk().n)) \(w.get(1).s)");<0};
EOF
rm -f "${WORK}/r15.bin"
( cd "${WORK}" && tk_build "${TKC}" a_lib.tk r15.tk -o r15.bin >/dev/null 2>&1 ) || tk_build_failed $?
r15="$([ -x "${WORK}/r15.bin" ] && tk_run "${WORK}/r15.bin" 2>&1 | tr '\n' ' ')"
expect "15 well-typed in-place calls run" "${r15}" "3 xy 2 6 xy "

# 16: no fix field on these E4031s
f16="$(cd "${WORK}" && "${TKC}" --check n2.tk 2>&1 | grep -c '"fix"')"
expect "16 no fix field" "${f16}" "0"

echo "C1480: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
