#!/usr/bin/env bash
# C125_set_out_of_range_traps.sh — `.set` outside [0, len) traps RT003
# (story 127.217, ADR-0012 a).
#
# THE DEFECT.  tk_array_set_w and tk_array_set_inplace_w returned the array
# unchanged for an out-of-range index, so `a=a.set(5;v)` on a 2-element array
# was a silent no-op while `a.get(5)` trapped RT003.  Codegen now emits the
# same bounds check before either call.
#
# WHAT THIS DRIVES.  Both lowerings — the copying `let c=b.set(i;v)` and the
# in-place self-update `a=a.set(i;v)` (114.18) — past the end, below zero,
# and on an empty array, at -O0 and -O2; plus the in-range case, which must
# still write the element.  The index comes from a function call so no pass
# can fold it.
#
# Negative control: delete the two emit_set_bounds_check() calls in
# src/llvm.c — every trap case fails (exit 0, the FAIL line prints).
#
# Story: 127.217

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
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c125_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C125: .set out of range traps RT003"
echo "-----------------------------------"

# prog NAME BODY — a program whose main runs BODY after `f=ix(n)` = n.
prog() {
    cat > "${WORK}/$1.tk" <<EOF
m=c125;
i=io:std.io;
f=ix(n:i64):i64{<n};
f=main():i64{
$2
  <0
};
EOF
}

# name | body | want exit | want stderr prefix | want stdout
prog copy_high  '  let b=@(1;2);
  let c=b.set(ix(5);9);
  io.println("FAIL copy .set past the end returned (len \(c.len))");'
prog copy_neg   '  let b=@(1;2);
  let c=b.set(ix(0-1);9);
  io.println("FAIL copy .set below zero returned (len \(c.len))");'
prog inpl_high  '  let a=mut.@(1;2);
  a=a.set(ix(2);9);
  io.println("FAIL in-place .set past the end returned (len \(a.len))");'
prog inpl_empty '  let a=mut.@();
  a=a.append(1);
  a=a.set(ix(0);4);
  let e=mut.@(0);
  e=e.slice(0;0);
  e=e.set(ix(0);4);
  io.println("FAIL .set on an empty array returned (len \(e.len))");'
prog in_range   '  let a=mut.@(1;2);
  a=a.set(ix(1);9);
  let b=@(1;2);
  let c=b.set(ix(0);7);
  io.println("\(a.get(1)) \(c.get(0)) \(b.get(0))");'

check() {  # check NAME WANT_EXIT WANT_ERR WANT_OUT
    local n="$1" we="$2" werr="$3" wout="$4" o
    for o in -O0 -O2; do
        if ! "${TKC}" --allow-all "${o}" --out "${WORK}/${n}" "${WORK}/${n}.tk" >"${WORK}/${n}.b" 2>&1; then
            fail "${n} ${o}: did not build: $(head -c 200 "${WORK}/${n}.b")"; continue
        fi
        "${WORK}/${n}" >"${WORK}/${n}.out" 2>"${WORK}/${n}.err" </dev/null
        local rc=$?
        local out; out="$(cat "${WORK}/${n}.out")"
        if [ "${rc}" = "${we}" ] && [ "${out}" = "${wout}" ] &&
           { [ -z "${werr}" ] || awk -v p="${werr}" 'index($0,p)==1{f=1} END{exit !f}' "${WORK}/${n}.err"; }; then
            pass "${n} ${o}: exit ${rc}${werr:+, ${werr}}"
        else
            fail "${n} ${o}: exit ${rc} (want ${we}), stdout [${out}] (want [${wout}]), stderr [$(head -1 "${WORK}/${n}.err")]"
        fi
    done
}

check copy_high  1 "RT003: index 5 out of bounds for length 2"  ""
check copy_neg   1 "RT003: index -1 out of bounds for length 2" ""
check inpl_high  1 "RT003: index 2 out of bounds for length 2"  ""
check inpl_empty 1 "RT003: index 0 out of bounds for length 0"  ""
check in_range   0 ""                                            "9 7 1"

echo ""
echo "C125: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
