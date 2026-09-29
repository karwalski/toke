#!/usr/bin/env bash
# C297_recursive_struct_resolves.sh — a struct with a field of its own type
# (through an array) type-checks instead of overflowing the stack (story
# 138.13, loke T-52).
#
# resolve_type resolved field `c:@$n` by resolving `$n`, whose field c
# resolved `$n`, ... until tkc died with SIGSEGV (exit 139) at --check.  A
# cycle through an array or pointer field is a legal recursive type: the
# inner reference is now the very Type being built, so a tree node checks,
# and field access on an element resolves.
#
# Negative control: TKC=<main's toke> -- cases 1-3 fail (rc=139).
#
# Story: 138.13 (T-52)

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
rc() { printf '%s\n' "$1" > "${WORK}/p.tk"; "${TKC}" --check "${WORK}/p.tk" >/dev/null 2>&1; echo "rc=$?"; }

expect "1 the loke T-52 tree node" \
    "$(rc 'm=rb;t=$n{v:i32;c:@$n};f=mk():$n{<$n{v:1 as i32;c:@()}};')" "rc=0"
expect "2 field access through the recursive element" \
    "$(rc 'm=rb;t=$n{v:i64;c:@$n};f=sum(x:$n):i64{let t=mut.x.v;lp(let i=0;i<x.c.len;i=i+1){t=t+sum(x.c.get(i))};<t};f=main():i64{let a=$n{v:2;c:@()};let b=$n{v:3;c:@(a;a)};<sum(b)+b.c.get(1).v};')" "rc=0"
expect "3 mutual recursion through two structs" \
    "$(rc 'm=rb;t=$a{bs:@$b};t=$b{xs:@$a;k:i64};f=g(x:$a):i64{<x.bs.get(0).xs.get(0).bs.get(0).k};f=main():i64{<0};')" "rc=0"
expect "4 a missing field on the recursive element is still E4025" \
    "$(rc 'm=rb;t=$n{v:i64;c:@$n};f=g(x:$n):i64{<x.c.get(0).nope};f=main():i64{<0};')" "rc=1"

printf 'm=rb;i=io:std.io;t=$n{v:i64;c:@$n};f=sum(x:$n):i64{let t=mut.x.v;lp(let i=0;i<x.c.len;i=i+1){t=t+sum(x.c.get(i))};<t};f=main():i64{let a=$n{v:2;c:@()};let b=$n{v:3;c:@(a;a)};io.println("\\(sum(b)) \\(b.c.get(1).v)");<0};\n' > "${WORK}/r.tk"
if "${TKC}" "${WORK}/r.tk" -o "${WORK}/r.bin" >/dev/null 2>&1; then
    expect "5 the tree builds and sums" "$("${WORK}/r.bin")" "7 2"
else expect "5 the tree builds and sums" "build failed" "7 2"; fi

echo "C297: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
