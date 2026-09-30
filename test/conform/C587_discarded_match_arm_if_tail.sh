#!/usr/bin/env bash
# C587_discarded_match_arm_if_tail.sh — a one-sided match in a branch tail of
# an expression-if that is the arm body of a DISCARDED match is a statement,
# not a used value (story 127.252 residue; follows C533 / C561).
#
# THE DEFECT.  types.c passes "discarded" from a statement match to an arm
# body that is itself a match (127.195), and from a statement `if` to its
# branches' tails (127.252 (2)) -- but not from a statement match to an arm
# body that is an expression-if.  So in
#     mt g(1) {$ok:v if(c){ mt g(0-1) {$ok:w io.println("w")} }el{ .. }; ..};
# standing as a statement, the inner one-sided match drew E4010 "the match's
# value is used", which is false: nothing uses it.  Measured at main
# f605aec9: E4010, the program refused.  (The residue as filed -- "a
# block-bodied arm of a USED match whose tail is a one-sided match" -- is
# already diagnosed at HEAD; a match arm's body is an expression, so its
# only block-bodied form is an expression-if.  Cases 4-6 pin that.)
#
# WHAT THIS DRIVES.  1-3: discarded outer match, arm body an expression-if
# (then-tail, else-tail after a statement, an el-if chain) with a one-sided
# tail match: --check clean, and the build runs.  4-6: the same arm body
# under a USED match (let-bound, returned, with a statement before the
# tail): E4010 on the inner match, no fix.  7: both sides present: clean.
#
# Negative control: TKC=<main f605aec9's toke> — cases 1-3 fail (E4010).
# Or delete the `if_discard=body` line in the NODE_MATCH_ARM loop of
# src/types.c — the same.
#
# Story: 127.252 (residue)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c587_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C587: discarded match arm -> expression-if tail is a statement"
echo "--------------------------------------------------------------"

HEAD='m=c587;
i=io:std.io;
t=$ue{$bad:str};
f=g(n:i64):i64!$ue{ if(n<0){ <$bad("x") }; <n };'

# clean NAME BODY WANT_STDOUT — --check clean; the build prints WANT_STDOUT.
clean() {
    printf '%s\n%s\n' "${HEAD}" "$2" > "${WORK}/$1.tk"
    local rc=0
    "${TKC}" --check "${WORK}/$1.tk" >"${WORK}/$1.o" 2>&1 || rc=$?
    if [ "${rc}" != 0 ]; then
        fail "$1: --check rc ${rc}: $(grep -o '"error_code":"[A-Z0-9]*"\|"message":"[^"]*"' "${WORK}/$1.o" | head -2 | tr '\n' ' ')"
        return
    fi
    rc=0
    "${TKC}" --out "${WORK}/$1" "${WORK}/$1.tk" >"${WORK}/$1.b" 2>&1 || rc=$?
    local out; out="$("${WORK}/$1" 2>&1)"
    if [ "${rc}" = 0 ] && [ "${out}" = "$3" ]; then pass "$1: clean, runs, prints [$3]"
    else fail "$1: build rc ${rc}, printed [${out}], want [$3]"; fi
}

# e4010 NAME BODY LINE — exactly one E4010 on LINE, no fix.
e4010() {
    printf '%s\n%s\n' "${HEAD}" "$2" > "${WORK}/$1.tk"
    local rc=0
    "${TKC}" --check "${WORK}/$1.tk" >"${WORK}/$1.o" 2>&1 || rc=$?
    local n; n="$(grep -c '"error_code":"E' "${WORK}/$1.o")"
    if [ "${rc}" = 1 ] && [ "${n}" = 1 ] && grep -q '"error_code":"E4010"' "${WORK}/$1.o" &&
       grep -q "\"line\":$3," "${WORK}/$1.o" && ! grep -q '"fix"' "${WORK}/$1.o"; then
        pass "$1: one E4010 on line $3, no fix"
    else
        fail "$1: rc ${rc}, ${n} error(s): $(head -c 240 "${WORK}/$1.o")"
    fi
}

clean 1_then_tail 'f=main():i64{ let c=true;
  mt g(1) {$ok:v if(c){ mt g(0-1) {$ok:w io.println("w")} }el{ io.println("n") }; $err:e io.println("e")};
  io.println("end"); <0 };' "end"
clean 2_else_tail 'f=main():i64{ let c=false;
  mt g(1) {$ok:v if(c){ io.println("t") }el{ io.println("s"); mt g(2) {$ok:w io.println("w")} }; $err:e io.println("e")};
  <0 };' "s
w"
clean 3_elif_chain 'f=main():i64{ let n=2;
  mt g(1) {$ok:v if(n==1){ io.println("1") }el if(n==2){ mt g(0-1) {$err:x io.println("x")} }el{ io.println("3") }; $err:e io.println("e")};
  <0 };' "x"
e4010 4_used_let 'f=main():i64{ let c=true;
  let a=mt g(1) {$ok:v if(c){ mt g(0-1) {$ok:w w} }el{ 7 }; $err:e 0};
  <a };' 6
e4010 5_used_return 'f=h(c:bool):i64{
  <mt g(1) {$ok:v if(c){ mt g(0-1) {$ok:w w} }el{ 7 }; $err:e 0} };
f=main():i64{ <h(true) };' 6
e4010 6_used_after_stmt 'f=main():i64{ let c=true;
  let a=mt g(1) {$ok:v if(c){ io.println("s"); mt g(0-1) {$ok:w w} }el{ 7 }; $err:e 0};
  <a };' 6
clean 7_both_sides 'f=main():i64{ let c=true;
  let a=mt g(1) {$ok:v if(c){ mt g(0-1) {$ok:w w; $err:x 5} }el{ 7 }; $err:e 0};
  io.println("\(a)"); <0 };' "5"

echo ""
echo "C587: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
