#!/usr/bin/env bash
# C584_nesting_limit_e9010.sh — deeply nested source is one located E9010,
# not a stack-overflow crash (story 121.28, audit-120 compiler-frontend
# COM-02).
#
# THE DEFECT.  The parser is recursive descent and every pass after it walks
# the tree recursively, with no depth bound anywhere.  Measured at main
# f605aec9 with `--check`: 10000 nested '(' , 10000 prefix '-', 10000 '@'
# type prefixes and 2000 nested `if(..){` blocks each died with SIGSEGV
# (rc 139) and no diagnostic; a 10000-term `0+1+1+…` chain -- a left-deep
# tree with no brackets -- did too, and 3000 terms crashed the build.
#
# NOW: parse() bounds bracket nesting and runs of prefix operators / type
# prefixes on the token stream (TKC_MAX_NEST_DEPTH = 256) and the height of
# the finished tree without recursion (TKC_MAX_AST_DEPTH = 1000).  Either is
# ONE E9010 naming the limit, located at the offending token or node, no fix.
#
# WHAT THIS DRIVES.  Each crash shape well past the limit (one E9010, rc 1,
# located on line 2), and each shape just under the limit (clean --check,
# and a build that runs).
#
# Negative control: TKC=<main f605aec9's toke> — every over-limit case fails
# (rc 139, no record).  Or make nest_prescan and ast_too_deep return 0 in
# src/parser.c — the same.
#
# Story: 121.28

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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c584_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C584: nesting past the limit is one E9010, not a crash"
echo "------------------------------------------------------"

# gen NAME SHAPE N — write NAME.tk with SHAPE nested N deep.
gen() {
    python3 - "$1" "$2" "$3" "${WORK}" <<'PY'
import sys
name, shape, n, work = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
body = {
    "paren": "f=main():i64{let x=" + "(" * n + "1" + ")" * n + "; <x};",
    "unary": "f=main():i64{let x=0" + "-" * n + "1; <x};",
    "type":  "f=g(a:" + "@" * n + "i64):i64{<0};\nf=main():i64{<0};",
    "block": "f=main():i64{" + "if(1==1){" * n + "let y=1;" + "};" * n + "<0};",
    "chain": "f=main():i64{let x=0" + "+1" * n + "; <x%7};",
}[shape]
open("%s/%s.tk" % (work, name), "w").write("m=c584;\n" + body + "\n")
PY
}

# over NAME SHAPE N WHAT — --check must give exactly one E9010, on line 2.
over() {
    gen "$1" "$2" "$3"
    local rc=0
    "${TKC}" --check "${WORK}/$1.tk" >"${WORK}/$1.o" 2>&1 || rc=$?
    local n9 nall
    n9="$(grep -c '"error_code":"E9010"' "${WORK}/$1.o")"
    nall="$(grep -c '"error_code":' "${WORK}/$1.o")"
    if [ "${rc}" = 1 ] && [ "${n9}" = 1 ] && [ "${nall}" = 1 ] &&
       grep -q "\"line\":2," "${WORK}/$1.o" &&
       grep -q "compiler limit exceeded: $4 deeper than" "${WORK}/$1.o" &&
       ! grep -q '"fix"' "${WORK}/$1.o"; then
        pass "$1: one located E9010 ($4), no fix"
    else
        fail "$1: rc ${rc}, ${n9} E9010 of ${nall} records: $(head -c 200 "${WORK}/$1.o")"
    fi
}

# under NAME SHAPE N — --check is clean and the build runs.
under() {
    gen "$1" "$2" "$3"
    local rc=0
    "${TKC}" --check "${WORK}/$1.tk" >"${WORK}/$1.o" 2>&1 || rc=$?
    if [ "${rc}" != 0 ]; then fail "$1: --check rc ${rc}: $(head -c 200 "${WORK}/$1.o")"; return; fi
    rc=0
    "${TKC}" --out "${WORK}/$1" "${WORK}/$1.tk" >"${WORK}/$1.b" 2>&1 || rc=$?
    if [ "${rc}" = 0 ] && [ -x "${WORK}/$1" ]; then
        "${WORK}/$1" >/dev/null 2>&1
        pass "$1: under the limit checks and builds"
    else
        fail "$1: build rc ${rc}: $(head -c 200 "${WORK}/$1.b")"
    fi
}

over  paren_over  paren 10000 "brackets nested"
over  unary_over  unary 10000 "prefix operators repeated"
over  type_over   type  10000 "type prefixes repeated"
over  block_over  block 2000  "brackets nested"
over  chain_over  chain 10000 "expression or statement nesting"

under paren_under paren 250
under unary_under unary 250
under type_under  type  250
under block_under block 120
under chain_under chain 990

echo ""
echo "C584: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
