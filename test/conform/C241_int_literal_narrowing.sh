#!/usr/bin/env bash
# C241_int_literal_narrowing.sh — an integer literal narrows to a declared
# integer type when its value fits (story 127.230 item 4).
#
# `let b:u8=5`, `let n:u64=0` and `let a:@(u8)=@(1;2)` were E4031: a literal
# is i64 by default and i64 widens to nothing narrower.  The binary-operator
# path already let a literal adopt the other operand's integer type; the
# value-flow paths (let annotation, assignment, argument, return, array
# store) did not.  A literal that does NOT fit stays E4031, and the program
# that does fit must print the values it wrote.
#
# Negative control: TKC=<main's toke> -- cases 1-6, 10 and 11 fail (E4031);
# 12-14 pass on main only because `byte` was unresolved there (@(?)).
#
# Story: 127.230 (item 4)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|got of each diagnostic joined by ' / '; "rc=N" when there are none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s" % (d.get("error_code"), d.get("got")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

expect "1 let u8 / u64 / i32 from a fitting literal" \
    "$(diags 'm=q;f=main():i64{let b:u8=5;let n:u64=0;let c:i32=-7;<0};')" "rc=0"
expect "2 let @(u8) from an array literal that fits" \
    "$(diags 'm=q;f=main():i64{let a:@(u8)=@(1;255);let z:@(@(u8))=@(@(1);@(2;3));<0};')" "rc=0"
expect "3 a literal argument into a u8 parameter" \
    "$(diags 'm=q;f=g(x:u8):u8{<x};f=main():i64{let r=g(200);<0};')" "rc=0"
expect "4 a negative literal returned as i32" \
    "$(diags 'm=q;f=h():i32{<-5};f=main():i64{<0};')" "rc=0"
expect "5 assignment of a literal to a u16 binding" \
    "$(diags 'm=q;f=main():i64{let v:u16=mut.1;v=65535;<0};')" "rc=0"
expect "6 append a literal onto a @(u8)" \
    "$(diags 'm=q;f=main():i64{let a:@(u8)=mut.@(1);a=a.append(9);<0};')" "rc=0"
expect "7 out of range for u8: still E4031" \
    "$(diags 'm=q;f=main():i64{let z:u8=300;<0};')" "E4031|i64"
expect "8 negative into u64: still E4031" \
    "$(diags 'm=q;f=main():i64{let w:u64=-1;<0};')" "E4031|i64"
expect "9 one element out of range: the array is still E4031" \
    "$(diags 'm=q;f=main():i64{let a:@(u8)=@(1;256);<0};')" "E4031|@(i64)"
expect "10 i8 lower bound -128 fits, -129 does not" \
    "$(diags 'm=q;f=main():i64{let a:i8=-128;let b:i8=-129;<0};')" "E4031|i64"

# `byte` resolves to u8 (127.220 item 4), so a @byte's elements are u8: they
# widen to i64 like u64 does, and take part in bitwise and concat operators
# with literals (bench CRY-041).
expect "12 u8 element into i64 arithmetic" \
    "$(diags 'm=q;f=g(w:@byte;i:i64):i64{let r=w.get(i);<r*256+i};f=main():i64{<0};')" "rc=0"
expect "13 bitwise on u8 elements and literals" \
    "$(diags 'm=q;f=g(b:@byte;o:i64):i64{<(b.get(o)|(b.get(o+1)<<8))&255};f=main():i64{<0};')" "rc=0"
expect "14 literal array concatenated onto a @byte" \
    "$(diags 'm=q;f=g(b:@byte):@byte{let p=mut.b;p=p+@(128);<p};f=main():i64{<0};')" "rc=0"

# The accepted program prints what it wrote.
cat > "${WORK}/r.tk" <<'TK'
m=q;i=io:std.io;i=j:std.json;
f=g(x:u8):u8{<x};
f=h():i32{<-5};
f=main():i64{let b:u8=5;let a:@(u8)=@(1;255);let n:u64=0;j.print(a);io.println("\(b) \(g(200)) \(h()) \(n) \(a.get(1))");<0};
TK
if "${TKC}" "${WORK}/r.tk" -o "${WORK}/r.bin" >/dev/null 2>&1; then
    expect "15 runtime values" "$("${WORK}/r.bin" | tr '\n' ' ')" "[1,255] 5 200 -5 0 255 "
else
    expect "15 runtime values" "build failed" "[1,255] 5 200 -5 0 255 "
fi

echo "C241: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
