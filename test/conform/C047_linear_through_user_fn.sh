#!/usr/bin/env bash
# C047_linear_through_user_fn.sh — a linear container stays linear when it is
# passed to a user function that only reads it.
#
# Story: 127.108.
#
# THE DEFECT.  The catalogue's canonical key-value pattern
# (patterns/parse-kv-lines/a.tk) builds a map with `m=m.set(k;v)` and then
# passes it to a user function `chk(m)` that only reads it.  The linearity
# analysis (114.18 / ADR-0006) counted ANY bare argument to a user function
# as alias-creating, so the map never entered the linear set and every
# `m.set` lowered to the COPYING tk_map_set_w: O(N^2) time and O(N^2) live
# memory (9.87 s / 2.5 GB at N=32,000 against 0.02 s / 12 MB inlined).  The
# gate that decided it, la_nonretaining_callee, was a three-name whitelist of
# stdlib modules (str, io, fmt).  Arrays refused identically.
#
# THE RULE.  Whether a callee retains its argument is a property of the
# callee's body, so it is derived from the body.  A bare argument `f(x)` to a
# top-level user function `f` of the same module creates no alias when f's
# corresponding parameter passes the same use analysis in STRICT mode: a
# terminal `<p` return is a retention there (the caller gets the block back),
# a closure or spawn that mentions p is a retention, binding or storing p is
# a retention, and passing p on is accepted only when THAT callee's
# parameter passes too (transitively, depth-limited).  A recursive use is
# answered on the assumption under test (127.171: a callee whose recursive
# uses are themselves read-only is non-retaining; C224 holds the retaining
# recursion control).  A call name that the caller also binds locally is not resolved
# to the top-level function.
#
# WHY THE CONTROLS MATTER MORE THAN THE WIN.  A wrong non-retention proof
# silently resurrects 127.55: in-place mutation of an aliased container.  So
# every retaining shape below asserts BOTH that the copying path is kept AND
# the value a wrongly-in-place build would corrupt.

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

WORK="$(mktemp -d /tmp/tkc_c047_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C047: a linear container passed to a read-only user function stays linear"
echo "--------------------------------------------------"

# case NAME MODE EXPECTED SOURCE
#   MODE inplace — the IR must call an *_inplace_w setter/appender and no
#                  copying tk_map_set_w / tk_array_set_w / tk_array_append_w
#   MODE copying — the IR must call NO *_inplace_w
#   and the program must print exactly EXPECTED.
case_() {
    local name="$1" mode="$2" expected="$3" src="$4"
    printf '%s\n' "${src}" > c.tk
    rm -f c.ll c_bin
    if ! "${TKC}" --emit-llvm --out c.ll c.tk >compile.log 2>&1 || [ ! -f c.ll ] \
       || ! "${TKC}" --allow-all --out c_bin c.tk >>compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | tail -3
        FAIL=$((FAIL + 1)); return
    fi
    local inpl copy out rc=0
    inpl=$(grep -c 'call .*@tk_[a-z_]*_inplace_w(' c.ll)
    copy=$(grep -cE 'call .*@tk_(map_set|array_set|array_append)_w\(' c.ll)
    out="$(./c_bin 2>&1)" || rc=$?
    local ok=1 why=""
    if [ "${mode}" = inplace ]; then
        { [ "${inpl}" -ge 1 ] && [ "${copy}" -eq 0 ]; } || { ok=0; why="expected in-place (inplace=${inpl}, copying=${copy})"; }
    else
        [ "${inpl}" -eq 0 ] || { ok=0; why="expected the copying path (inplace=${inpl}, copying=${copy})"; }
    fi
    if [ "${out}" != "${expected}" ]; then ok=0; why="${why} printed '${out}' (rc ${rc}), expected '${expected}'"; fi
    if [ "${ok}" -eq 1 ]; then
        echo "  PASS: ${name} — ${mode}, '${expected}'"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: ${why}"
        FAIL=$((FAIL + 1))
    fi
}

# ── the story: read-only callees no longer demote the container ─────────────
case_ 'map passed to a read-only fn (parse-kv-lines/a shape)' inplace 't=3000004' \
'm=main;
i=io:std.io;
i=s:std.str;
f=chk(m:@(str:str)):i64{
  let ks=m.keys();
  let t=mut.ks.len*1000000;
  lp(let i=0;i<ks.len;i=i+1){t=t+s.len(m.get(ks.get(i)))};
  <t
};
f=main():i64{
  let m=mut.@("":"");
  m=m.set("a";"xy");
  m=m.set("b";"zw");
  io.println("t=\(chk(m))");
  <0
};'

case_ 'array passed to a read-only fn' inplace 't=10' \
'm=main;
i=io:std.io;
f=tot(a:@i64):i64{let t=mut.0;lp(let i=0;i<a.len;i=i+1){t=t+a.get(i)};<t};
f=main():i64{
  let a=mut.@(0);
  lp(let i=0;i<5;i=i+1){a=a.append(i)};
  io.println("t=\(tot(a))");
  <0
};'

case_ 'transitive: read-only fn passes it to another read-only fn' inplace '14 7' \
'm=main;
i=io:std.io;
f=sum(a:@i64):i64{let t=mut.0;lp(let i=0;i<a.len;i=i+1){t=t+a.get(i)};<t};
f=tot(a:@i64):i64{<sum(a)};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let s=tot(a);
  a=a.set(1;7);
  io.println("\(s) \(a.get(1))");
  <0
};'

# ── the controls: every retaining shape keeps the copying path ──────────────
case_ 'callee returns its parameter' copying '9 2 7' \
'm=main;
i=io:std.io;
f=id(a:@i64):@i64{<a};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let b=id(a);
  a=a.set(1;7);
  io.println("\(b.get(0)) \(b.get(1)) \(a.get(1))");
  <0
};'

case_ 'callee returns it via another fn (transitive retention)' copying '9 2 7' \
'm=main;
i=io:std.io;
f=id(a:@i64):@i64{<a};
f=pass(a:@i64):@i64{<id(a)};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let b=pass(a);
  a=a.set(1;7);
  io.println("\(b.get(0)) \(b.get(1)) \(a.get(1))");
  <0
};'

case_ 'callee stores it in a container literal' copying '2 7' \
'm=main;
i=io:std.io;
f=keep(a:@i64):@@i64{<@(a)};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let b=keep(a);
  a=a.set(1;7);
  io.println("\(b.get(0).get(1)) \(a.get(1))");
  <0
};'

case_ 'callee binds it to a local and returns that' copying '2 7' \
'm=main;
i=io:std.io;
f=id(a:@i64):@i64{let q=a;<q};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let b=id(a);
  a=a.set(1;7);
  io.println("\(b.get(1)) \(a.get(1))");
  <0
};'

case_ 'callee captures it in a closure' copying '2 7' \
'm=main;
i=io:std.io;
f=cap(a:@i64):i64{let g=fn(k:i64):i64{<a.get(k)};<g(1)};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let s=cap(a);
  a=a.set(1;7);
  io.println("\(s) \(a.get(1))");
  <0
};'

case_ 'map: callee returns its parameter' copying 'v1 v2' \
'm=main;
i=io:std.io;
f=idm(m:@(str:str)):@(str:str){<m};
f=main():i64{
  let m=mut.@("":"");
  m=m.set("k";"v1");
  let h=idm(m);
  m=m.set("k";"v2");
  io.println("\(h.get("k")) \(m.get("k"))");
  <0
};'

case_ 'read-only recursive callee stays linear (127.171)' inplace '9 7' \
'm=main;
i=io:std.io;
f=rec(a:@i64;n:i64):i64{if(n==0){<a.get(0)};<rec(a;n-1)};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let s=rec(a;3);
  a=a.set(1;7);
  io.println("\(s) \(a.get(1))");
  <0
};'

case_ 'a local binding shadows the fn name: not resolved to the top-level fn' copying '102 7' \
'm=main;
i=io:std.io;
f=sum(a:@i64):i64{<a.get(0)};
f=main():i64{
  let a=mut.@(1;2;3);
  a=a.set(0;9);
  let sum=fn(x:@i64):i64{<x.get(1)+100};
  let b=sum(a);
  a=a.set(1;7);
  io.println("\(b) \(a.get(1))");
  <0
};'

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
