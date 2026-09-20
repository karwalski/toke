#!/usr/bin/env bash
# C015_expr_if_str_append.sh — a str bound from an expression-`if` (or `mt`)
# survives being appended to an array (story 127.51).
#
# This defect is CLOSED.  It was closed by commit 8934347 ("127.80:
# interpolation asks the interface file, not a hand-written name list"), which
# fixed it as a side effect rather than deliberately, and it was never pinned.
# This file pins it, because the closing change was incidental and nothing else
# in the tree asserts the behaviour.
#
# Mechanism, for anyone who has to re-open it: `expr_struct_type()` in
# src/llvm.c had no NODE_IF_STMT / NODE_MATCH_STMT case, though its sibling
# `expr_llvm_type()` has had one since 114.x.  A `str` bound from an
# expression-`if` therefore arrived at its use site UNTAGGED — the compiler had
# not established the type, so it substituted the default — and the array
# append stored an untyped slot.  8934347 adds both cases, taking the branch
# tail's type, mirroring expr_llvm_type.
#
# This is the same root as 127.7 / 127.10 / 127.24: a compiler substituting a
# default for something it never established.  The symptom varied — a printed
# pointer, a dropped element, a name never resolved — the cause did not.
#
# Observed history of this exact repro:
#   e9746f2 (2026-09-18)  len=3, elements are pointers (4310326432, ...)
#   929ba71 (2026-09-19)  exit 139 — segfault, no output at all
#   8934347 (2026-09-19)  len=2, alpha, beta          <- closed here
#
# Stories: 127.51 (closed by 8934347, filed under 127.80)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_exprifstr_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C015: an expression-if-bound str appends as a string"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE — asserts stdout at -O0/-O1/-O2/-O3.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "case.tk"
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f case_bin
        if ! "${TKC}" --allow-all "${o}" --out "case_bin" "case.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log
            FAIL=$((FAIL + 1))
            continue
        fi
        out="$(./case_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${o}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected '${expected}', got '${out}' (rc ${rc})"
            FAIL=$((FAIL + 1))
        fi
    done
}

# ── Case 1: the original repro ───────────────────────────────────────────
# The assertion is on the ELEMENT VALUES, not just a.len.  A test that checked
# only the length would have passed against the pointer-printing era of this
# defect, when len was right and every element was garbage.
run_case "expr-if-bound str appends, both branches" "len=2
e=alpha
e=beta" \
'm=main;
i=io:std.io;
f=main():i64{
  let a=mut.@();
  let c=true;
  let ty=if(c){"alpha"}el{"beta"};
  a=a+@(ty);
  let d=false;
  let t2=if(d){"alpha"}el{"beta"};
  a=a+@(t2);
  io.println("len=\(a.len)");
  lp(let i=0;i<a.len;i=i+1){
    io.println("e=\(a.get(i))")
  };
  <0
};'

# ── Case 2: the same through .append rather than +@() ────────────────────
run_case "expr-if-bound str survives .append" "n=1 v=yes" \
'm=main;
i=io:std.io;
f=main():i64{
  let a=mut.@();
  let c=true;
  let ty=if(c){"yes"}el{"no"};
  a=a.append(ty);
  io.println("n=\(a.len) v=\(a.get(0))");
  <0
};'

# ── Case 3: interpolating the binding directly ───────────────────────────
# The narrowest statement of the root cause: the binding must be known to be a
# str at its use site, or interpolation prints its address.
run_case "expr-if-bound str interpolates as a string" "ty=beta" \
'm=main;
i=io:std.io;
f=main():i64{
  let c=false;
  let ty=if(c){"alpha"}el{"beta"};
  io.println("ty=\(ty)");
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
