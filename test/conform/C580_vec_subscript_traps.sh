#!/usr/bin/env bash
# C580_vec_subscript_traps.sh — a std.vec subscript outside [0, len) traps
# RT003 (story 121.31, ADR-0012 a).
#
# THE DEFECT.  Every array subscript traps RT003 (124.2a, 127.217), but a
# std.vec handle is a DynArr, not an array block, so its `.get`/`.set` go to
# tk_vec_get_w / tk_vec_set_w — and tk_vec_get answered 0, tk_vec_set did
# nothing, for an index outside [0, len).  `v.get(5)` on a one-element vec
# printed 0 and exited 0; `vec.set(v;5;9)` was a silent no-op.  That is the
# silent-default class 127.217 removed from arrays.  Codegen now checks the
# index against tk_vec_len_w before every vec get/set, on all three
# spellings: the index form `v.get(i)`, the receiver form `v.set(i;x)`, and
# the module form `vec.get(v;i)` / `vec.set(v;i;x)`.
#
# WHAT THIS DRIVES.  Each spelling past the end and below zero, the empty
# vec, at -O0 and -O2; plus the in-range case, which must still read and
# write.  The index comes from a function call so no pass can fold it.
#
# Negative control: delete the three emit_len_bounds_check(c, "tk_vec_len_w",
# ...) calls in src/llvm.c — every trap case fails (exit 0, the FAIL line
# prints, e.g. "FAIL idx get returned 0").
#
# Story: 121.31

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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c580_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C580: std.vec subscript out of range traps RT003"
echo "------------------------------------------------"

# prog NAME BODY — main runs BODY with v = a one-element vec (1) and ix(n)=n.
prog() {
    cat > "${WORK}/$1.tk" <<EOF
m=c580;
i=io:std.io;
i=vec:std.vec;
f=ix(n:i64):i64{<n};
f=main():i64{
  let v=mut.vec.new();
  v=vec.push(v;1);
$2
  <0
};
EOF
}

prog idx_hi   '  let x=v.get(ix(5));
  io.println("FAIL idx get returned \(x)");'
prog idx_neg  '  let x=v.get(ix(0-1));
  io.println("FAIL idx get below zero returned \(x)");'
prog rcv_set  '  v=v.set(ix(1);9);
  io.println("FAIL receiver set past the end returned");'
prog mod_get  '  let x=vec.get(v;ix(1));
  io.println("FAIL module get returned \(x)");'
prog mod_set  '  v=vec.set(v;ix(0-1);9);
  io.println("FAIL module set below zero returned");'
prog empty    '  let e=vec.new();
  let x=e.get(ix(0));
  io.println("FAIL get on an empty vec returned \(x)");'
prog in_range '  v=vec.push(v;2);
  v=v.set(ix(1);7);
  v=vec.set(v;ix(0);5);
  io.println("\(v.get(ix(0))) \(vec.get(v;ix(1))) \(vec.len(v))");'

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

check idx_hi   1 "RT003: index 5 out of bounds for length 1"  ""
check idx_neg  1 "RT003: index -1 out of bounds for length 1" ""
check rcv_set  1 "RT003: index 1 out of bounds for length 1"  ""
check mod_get  1 "RT003: index 1 out of bounds for length 1"  ""
check mod_set  1 "RT003: index -1 out of bounds for length 1" ""
check empty    1 "RT003: index 0 out of bounds for length 0"  ""
check in_range 0 ""                                            "5 7 2"

echo ""
echo "C580: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
