#!/usr/bin/env bash
# C821_module_constant_check.sh — what --check says about a module constant.
#
# Story: 127.310.
#
# Before 127.310 the checker typed a use of a constant as unknown and never
# judged the declaration, so every one of the rejected programs below passed
# --check and then failed (or silently misbehaved) at build:
#   - `pi=1:f64;` -- an integer literal declared f64 (an annotated `let` with
#     the same literal is E4031);
#   - `w=5:$str;` -- a literal of the wrong kind;
#   - `b=300:u8;` -- an integer literal that does not fit the declared type
#     (`let b:u8=300` is E4031; `b=200:u8` narrows and is clean);
#   - `let s:$str=lim` with `lim` an i64 constant -- the use is of the
#     declared type;
#   - `lim=6;` in a function body -- semantics.md §4.2: DECL_CONST is not
#     mutable.  This was E9003 at clang ("use of undefined value '%lim'").
# None carries a `fix`: the grammar admits only a literal in a constant, so
# the `as` cast an annotated `let` mismatch offers would not parse, and a
# constant has no `mut` spelling to rewrite an assignment to.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_modconstchk_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C821: module constants at --check"
echo "---------------------------------"

# rejects <name> <code> <program>: exit non-zero, exactly that code, no fix.
rejects() {
    local name="$1" code="$2"
    printf '%s\n' "$3" > t.tk
    local out rc
    out="$("${TKC}" --check t.tk 2>&1)"; rc=$?
    local codes
    codes="$(printf '%s\n' "${out}" | grep -o '"error_code":"[A-Z][0-9]*"' | sort -u | tr '\n' ' ')"
    if [ "${rc}" -ne 0 ] && [ "${codes}" = "\"error_code\":\"${code}\" " ] \
       && ! printf '%s\n' "${out}" | grep -q '"fix"'; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: rc ${rc}, codes '${codes}' (want ${code}, no fix)"
        printf '%s\n' "${out}" | head -c 300 | sed 's/^/      /'; echo
        FAIL=$((FAIL + 1))
    fi
}

accepts() {
    local name="$1"
    printf '%s\n' "$2" > t.tk
    if "${TKC}" --check t.tk > out.log 2>&1; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: --check rejected it"
        head -c 300 out.log | sed 's/^/      /'; echo
        FAIL=$((FAIL + 1))
    fi
}

rejects "int literal declared f64" E4031 'm=a;
pi=1:f64;
f=main():i64{<0};'
rejects "int literal declared \$str" E4031 'm=a;
w=5:$str;
f=main():i64{<0};'
rejects "int literal that does not fit u8" E4031 'm=a;
b=300:u8;
f=main():i64{<0};'
rejects "i64 constant used as a \$str" E4031 'm=a;
lim=5:i64;
f=main():i64{let s:$str=lim;<0};'
rejects "assignment to a constant" E4070 'm=a;
lim=5:i64;
f=main():i64{lim=6;<lim};'

accepts "well-typed constants of every literal kind" 'm=a;
lim=5:i64;
b=200:u8;
w=7:i32;
pi=3.5:f64;
nm="x":$str;
dbg=false:bool;
f=main():i64{let s:$str=nm;let p:f64=pi;<lim};'
# A mut local of the constant's name is a different binding: assigning it is
# legal, and only the constant itself is protected.
accepts "assigning a mut local that shadows a constant" 'm=a;
lim=5:i64;
f=main():i64{let lim=mut.1;lim=2;<lim};'

echo ""
echo "C821: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
