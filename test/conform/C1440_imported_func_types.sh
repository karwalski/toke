#!/usr/bin/env bash
# C1440_imported_func_types.sh -- a function-typed field, parameter or return
# imported through a .tki is a function value in the importing module
# (story 127.377).
#
# THE DEFECT: --emit-interface spells these types `(P;..):R` since 127.376,
# but the .tki type readers (types.c) never built a TY_FUNC from the
# spelling, so the importer held an untyped value: `let g=l.getf(); g(4)`
# printed 4 (the call lowered as its argument; the same module prints 5),
# `let c=l.mk().cb; c(4)` likewise, and `l.ap(1;20)` -- an integer where a
# function is called -- passed --check and died with SIGSEGV.
#
# NOW: the spelling is read into the same signature a function type written
# in the module gets (124.0c item 2), so the value is checked and called as
# a same-module function value is: an indirect call (cases 1-9), E4026 /
# E4031 on its calls and uses (cases 11-15), E4031 for a non-function
# argument where the interface declares a function type (case 10).  A field
# call `r.cb(4)` is E4036 exactly as in the same module (case 16).  An
# interface written before 127.376, which spelled these types "(", still
# loads and stays untyped, as before (cases 17-18).
#
# Negative control: TKC built from fix/z161-mt-iface-partial (6dca2454) --
# cases 1-3, 5-7, 9-15 red (13); 4, 8, 16-18 green on both.
#
# Story: 127.377

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
cat > "${WORK}/a_lib.tk" <<'EOF'
m=c1440.lib;
t=$rec{n:i64;cb:(i64):i64;fl:(f64):f64;two:(i64;i64):i64};
f=inc(x:i64):i64{<x+1};
f=dbl(x:i64):i64{<x*2};
f=half(x:f64):f64{<x/2.0};
f=add(a:i64;b:i64):i64{<a+b};
f=mk():$rec{<$rec{n:1;cb:inc;fl:half;two:add}};
f=mkadd(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
f=ap(h:(i64):i64;x:i64):i64{<h(x)};
f=getf():(i64):i64{<inc};
f=geth():(f64):f64{<half};
f=fns():@((i64):i64){<@(inc;dbl)};
f=hi(h:((i64):i64;i64):i64):i64{<h(inc;5)};
EOF
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
[ -f "${WORK}/c1440.lib.tki" ] || echo "  FAIL: setup: --emit-interface produced no c1440.lib.tki"

# run NAME BODY -- build BODY as the importer's main with a_lib.tk; print its output
run() {
    cat > "${WORK}/$1.tk" <<EOF
m=c1440.main;
i=io:std.io;
i=l:c1440.lib;
f=tri(x:i64):i64{<x*3};
f=app(h:(i64):i64;x:i64):i64{<h(x)};
f=main():i64{
$2
<0};
EOF
    rm -f "${WORK}/$1.bin"
    ( cd "${WORK}" && "${RUN}" 60 "${TKC}" a_lib.tk "$1.tk" -o "$1.bin" >/dev/null 2>&1 )
    if [ -x "${WORK}/$1.bin" ]; then "${RUN}" 20 "${WORK}/$1.bin" 2>&1 | tr '\n' ' '; echo "rc=${PIPESTATUS[0]}"
    else echo "no binary"; fi
}
# chk NAME BODY -- --check BODY as the importer; print the error codes and rc
chk() {
    cat > "${WORK}/$1.tk" <<EOF
m=c1440.main;
i=io:std.io;
i=l:c1440.lib;
f=takes(s:str):i64{<1};
f=main():i64{
$2
<0};
EOF
    local out rc
    out="$(cd "${WORK}" && "${TKC}" --check "$1.tk" 2>&1)"; rc=$?
    echo "$(printf '%s\n' "${out}" | grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' ')rc=${rc}"
}

expect "1 record field through a local" \
    "$(run r1 'let r=l.mk(); let c=r.cb; io.println("\(c(4))");')" "5 rc=0"
expect "2 field of a record returned in place" \
    "$(run r2 'let c=l.mk().cb; io.println("\(c(10))");')" "11 rc=0"
expect "3 returned function" \
    "$(run r3 'let g=l.getf(); io.println("\(g(4))");')" "5 rc=0"
expect "4 function, reference and closure arguments (control)" \
    "$(run r4 'io.println("\(l.ap(tri;5)) \(l.ap(&tri;6)) \(l.ap(fn(x:i64):i64{<x*5};5))");')" "15 18 25 rc=0"
expect "5 returned closure" \
    "$(run r5 'let a=l.mkadd(3); io.println("\(a(4)) \(l.ap(a;5))");')" "7 8 rc=0"
expect "6 f64 signatures, field and return" \
    "$(run r6 'let r=l.mk(); let fl=r.fl; let h=l.geth(); io.println("\(fl(5.0)) \(h(3.0))");')" "2.5 1.5 rc=0"
expect "7 two-parameter field" \
    "$(run r7 'let r=l.mk(); let tw=r.two; io.println("\(tw(2;3))");')" "5 rc=0"
expect "8 imported value passed on to a local function" \
    "$(run r8 'let g=l.getf(); io.println("\(app(g;7)) \(l.ap(g;8))");')" "8 9 rc=0"
expect "9 array of functions [(i64):i64]" \
    "$(run r9 'let a=l.fns(); let f1=a.get(1); io.println("\(f1(4))");')" "8 rc=0"

expect "10 non-function argument for a function parameter" \
    "$(chk n10 'io.println("\(l.ap(1;20))");')" "E4031 rc=1"
expect "11 argument type of a call through it" \
    "$(chk n11 'let g=l.getf(); io.println("\(g("x"))");')" "E4031 rc=1"
expect "12 argument count of a call through it" \
    "$(chk n12 'let r=l.mk(); let tw=r.two; io.println("\(tw(1))");')" "E4026 rc=1"
expect "13 bound to a str" \
    "$(chk n13 'let y:str=l.mk().cb;')" "E4031 rc=1"
expect "14 passed where a str is declared" \
    "$(chk n14 'let g=l.getf(); let z=takes(g);')" "E4031 rc=1"
expect "15 a function of another signature" \
    "$(chk n15 'io.println("\(l.ap(takes;1))");')" "E4031 rc=1"

# 16: field-call syntax answers as in the same module
cat > "${WORK}/s16.tk" <<'EOF'
m=s16;
i=io:std.io;
t=$rec{n:i64;cb:(i64):i64};
f=inc(x:i64):i64{<x+1};
f=mk():$rec{<$rec{n:1;cb:inc}};
f=main():i64{let r=mk();io.println("\(r.cb(4))");<0};
EOF
same="$(cd "${WORK}" && "${TKC}" --check s16.tk 2>&1 | grep -o '"error_code":"E[0-9]*"' | sort -u | tr '\n' ' ')"
imp="$(chk n16 'let r=l.mk(); io.println("\(r.cb(4))");')"
expect "16 r.cb(4) as in the same module" "${same}|${imp}" '"error_code":"E4036" |E4036 rc=1'

# 17-18: an interface from before 127.376 spelled every function type "("
mkdir -p "${WORK}/old"
sed 's/"([^"]*):[^"]*"/"("/g' "${WORK}/c1440.lib.tki" > "${WORK}/old/c1440.lib.tki"
cp "${WORK}/n10.tk" "${WORK}/r3.tk" "${WORK}/old/"
o17="$(cd "${WORK}/old" && "${TKC}" --check r3.tk >/dev/null 2>&1; echo "rc=$?")"
o18="$(cd "${WORK}/old" && "${TKC}" --check n10.tk >/dev/null 2>&1; echo "rc=$?")"
expect "17 old '(' spelling: loads, untyped" "$(grep -c '"):' "${WORK}/old/c1440.lib.tki") ${o17}" "0 rc=0"
expect "18 old '(' spelling: argument not judged, as before" "${o18}" "rc=0"

echo "C1440: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
