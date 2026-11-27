#!/usr/bin/env bash
# C2302_map_record_closure_result_type.sh — `.map` over a record-returning
# function, and over closures, is typed from the function value's declared
# return (story 127.550).
#
# THE DEFECT: a record map was tagged "@i64", so j.enc printed the records'
# addresses; a closure argument (a literal, one capturing a local, one bound
# by `let`) had no signature lookup at all and was "@i64" whatever it
# returned.
#
# THE FIX: types.c recv_map_fn_sig reads the signature the way a call through
# the value is judged: `&f` (func_ref_sig), a closure literal with a declared
# return (params_sig), a local holding a function value (fnval_bind_sig).
# A closure WITHOUT a declared return has no stated type and stays
# unmodelled (checked clean here; C2304 checks it adds no diagnostic).
#
# WHAT THIS CHECKS (runs): record map j.enc and field reads through `.get`;
# closure literal f64 and str maps, a capturing closure, a `let` closure.
#
# Negative control: main 58bd74e8 prints addresses and f64 bit patterns.
#
# Story: 127.550
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2302_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2302: .map over record functions and closures keeps the return type (127.550)"
echo "------------------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
t=$pt{x:i64;y:f64};
f=mk(x:i64):$pt{<$pt{x:x;y:0.5}};
f=main():i64{
  io.println(j.enc(@(1;2).map(&mk)));
  let ps=@(1;2).map(&mk);
  io.println("p1=\(ps.get(1).x),\(ps.get(1).y)");
  let fs=@(1.5;2.5);
  io.println(j.enc(fs.map(fn(v:f64):f64{<v+0.5})));
  io.println(j.enc(fs.map(fn(v:f64):str{<"s"})));
  let k=2.0;
  io.println(j.enc(fs.map(fn(v:f64):f64{<v*k})));
  let half=fn(v:f64):f64{<v/2.0};
  let hs=fs.map(half);
  io.println("h0=\(hs.get(0))");
  <0
};
TOKE

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  exp_r=$'[{"x":1,"y":0.5},{"x":2,"y":0.5}]\np1=2,0.5'
  exp_c=$'[2,3]\n["s","s"]\n[3,5]\nh0=0.75'
  got_r=$(echo "$out" | sed -n 1,2p); got_c=$(echo "$out" | sed -n 3,6p)
  [ "$rc" -eq 0 ] || fail "exit $rc"
  [ "$got_r" = "$exp_r" ] && pass "record map is @(\$pt)" || fail "record: $(echo "$got_r" | tr '\n' '|')"
  [ "$got_c" = "$exp_c" ] && pass "closure maps (literal, str, capture, let) typed" || fail "closures: $(echo "$got_c" | tr '\n' '|')"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2302: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
