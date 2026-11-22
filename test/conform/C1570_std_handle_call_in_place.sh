#!/usr/bin/env bash
# C1570_std_handle_call_in_place.sh -- a std call whose .tki return is a
# FIELD-LESS record (an opaque handle: `s.buf()` / `s.builder()` -> strbuf,
# `vec.new()` / push / set / tovec -> vechandle, `svg.rect(..)` and the other
# svg shapes -> svgelem, `securemem.alloc(..)` -> securebuf) is typed as that
# handle wherever it stands, not only when bound first (story 127.392).
#
# THE DEFECT: call_decl_ret_type gave the `let` binding the handle (127.66),
# but 127.386 adopted a std record in place only when it had fields, so the
# in-place call kept no type: `s.buf()+1`, `qs(s.buf())` (str parameter),
# `vec.new()+1`, `$loc{s:svg.rect(..)}` and `"\(vec.new())"` passed --check,
# where the bound form is E4031 / E4032; and `j.enc(vec.new())` printed the
# handle's address where the bound form is E9002 (no encoder for an opaque
# handle).
#
# NOW: every check case runs in place and bound by a `let` first; both must
# give the expected codes.  A handle is still integer-compatible
# (opaque_handle_type), so the idiomatic uses -- a handle into the std call
# that takes it, into an i64 parameter, in a record field or array of its own
# type -- stay clean and lower to the same IR (case 13 runs them).
#
# Negative control: TKC built from fix/z176-std-record-in-place (6d270505) --
# cases 1-10 and 14 red; 11-13, 15, 16 green.
#
# Story: 127.392

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

H='m=c1570;
i=io:std.io;
i=s:std.str;
i=vec:std.vec;
i=svg:std.svg;
i=sm:std.securemem;
i=j:std.json;
t=$loc{s:str};
f=qs(x:str):i64{<1};
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
    printf '%s\nf=main():i64{\nlet st=svg.style("red";"blue";1.0);\n%s\n<0};\n' "${H}" "${inp}" > "${WORK}/$1.tk"
    printf '%s\nf=main():i64{\nlet st=svg.style("red";"blue";1.0);\n%s\n<0};\n' "${H}" "${bnd}" > "${WORK}/$1b.tk"
    echo "$(codes "$1.tk") | $(codes "$1b.tk")"
}
SB='s.buf()'
VN='vec.new()'
SR='svg.rect(0.0;0.0;1.0;1.0;st)'
B31="E4031 rc=1 | E4031 rc=1"
B25="E4025 rc=1 | E4025 rc=1"
OK="rc=0 | rc=0"

expect "1 strbuf in arithmetic (the reported case)"  "$(chk n1 "${SB}" 'let a=X+1;')" "${B31}"
expect "2 strbuf as a str argument (reported)"       "$(chk n2 "${SB}" 'let a=qs(X);')" "${B31}"
expect "3 vechandle in arithmetic (reported)"        "$(chk n3 "${VN}" 'let a=X+1;')" "${B31}"
expect "4 vechandle as a str argument"               "$(chk n4 "${VN}" 'let a=qs(X);')" "${B31}"
expect "5 s.builder() as a str field"                "$(chk n5 's.builder()' 'let a=$loc{s:X};')" "${B31}"
expect "6 svgelem as a str argument"                 "$(chk n6 "${SR}" 'let a=qs(X);')" "${B31}"
expect "7 securebuf in arithmetic"                   "$(chk n7 'sm.alloc(16;0)' 'let a=X+1;')" "${B31}"
expect "8 vechandle interpolated"                    "$(chk n8 "${VN}" 'io.println("\(X)");')" "E4032 rc=1 | E4032 rc=1"
expect "9 field on a handle"                         "$(chk n9 'vec.push(vec.new();1)' 'let a=X.len;')" "${B25}"
expect "10 handle in an array beside a str"          "$(chk n10 "${SB}" 'let a=@("x";X);')" "${B31}"
expect "11 handle into its own std call / i64 (control)" \
    "$(chk p11 "${VN}" 'let a=vec.len(vec.push(X;1)); let b=qi(X); let c:i64=X;')" "${OK}"
expect "12 svgelem into svg.append / svg.group (control)" \
    "$(chk p12 "${SR}" 'let d=svg.append(svg.doc(2.0;2.0);svg.group(@(X;X);"g"));')" "${OK}"

# 13: well-typed in-place handles run as before
cat > "${WORK}/r13.tk" <<'EOF'
m=c1570r;
i=io:std.io;
i=s:std.str;
i=vec:std.vec;
i=svg:std.svg;
i=sm:std.securemem;
t=$hold{h:$vechandle};
f=mk():$vechandle{<vec.push(vec.new();7)};
f=main():i64{
let b=s.buf();
s.add(b;"ab");
let h=$hold{h:vec.push(vec.new();3)};
let hs=@(vec.new();mk());
let mp=@("k":mk());
let st=svg.style("red";"blue";1.0);
let d=svg.append(svg.doc(4.0;4.0);svg.group(@(svg.rect(0.0;0.0;1.0;1.0;st);svg.circle(1.0;1.0;1.0;st));"g"));
let x=vec.toarray(vec.tovec(@(4;5;6)));
io.println("\(s.done(b)) \(vec.len(vec.push(vec.push(vec.new();1);2))) \(vec.get(vec.set(vec.push(vec.new();1);0;9);0)) \(vec.len(h.h)) \(vec.len(hs.get(1))) \(vec.len(mt mp.get("k"){$ok:v v;$err:e vec.new()})) \(x.get(2)) \(sm.write(sm.alloc(16;0);"x")) \(s.len(svg.render(d))>40)");
<0};
EOF
rm -f "${WORK}/r13.bin"
( cd "${WORK}" && "${RUN}" 60 "${TKC}" r13.tk -o r13.bin >/dev/null 2>&1 )
r13="$([ -x "${WORK}/r13.bin" ] && "${RUN}" 20 "${WORK}/r13.bin" 2>&1 | tr '\n' ' ')"
expect "13 well-typed in-place handles run" "${r13}" "ab 2 9 1 1 1 6 true true "

# 14: j.enc of an in-place handle is refused as the bound form is, where it
# printed the handle's address.  It was E9002 (codegen: no encoder for an
# opaque handle); since 127.402 the checker reports it first as E4031.
jb() {
    printf 'm=c1570j;\ni=vec:std.vec;\ni=io:std.io;\ni=j:std.json;\nf=main():i64{\n%s\n<0};\n' "$2" > "${WORK}/$1.tk"
    local out; out="$(cd "${WORK}" && "${RUN}" 60 "${TKC}" "$1.tk" -o "$1.bin" 2>&1)"
    printf '%s\n' "${out}" | grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' '
}
expect "14 j.enc in place refused as bound" \
    "$(jb j14 'io.println(j.enc(vec.new()));') | $(jb j14b 'let v=vec.new(); io.println(j.enc(v));')" \
    "E4031  | E4031 "

# 15: no fix field on these diagnostics
f15="$(cd "${WORK}" && "${TKC}" --check n2.tk 2>&1 | grep -c '"fix"')"
expect "15 no fix field" "${f15}" "0"

# 16: a fielded std record in place is still typed (127.386 unchanged)
expect "16 fielded std record in place (unchanged)" \
    "$(chk u16 'svg.style("a";"b";1.0)' 'let a=X.nosuch;')" "${B25}"

echo "C1570: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
