#!/usr/bin/env bash
# C2301_map_bool_str_i64_result_type.sh — `.map` over bool, str and i64
# functions is typed @bool / @str / @i64 (story 127.550).
#
# THE DEFECT: the 127.44 fallback tag looked the mapped function up by its
# bare name and never found it, so every map was "@i64": a str map encoded
# its string pointers as numbers ([47110423352,...]) and printed a pointer
# for `"\(xs.map(&sh).get(1))"`; a bool map encoded and interpolated 0/1
# where an @bool prints false/true (127.194).
#
# WHAT THIS CHECKS (runs): j.enc and interpolation of `.get(i)` for each of
# the three element kinds, the i64 case as the unchanged control.
#
# Negative control: main 58bd74e8 prints [0,1,0], 1, pointers for the str
# map (the i64 lines alone are right).
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2301_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2301: .map over bool / str / i64 functions keeps the return type (127.550)"
echo "--------------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
i=s:std.str;
f=pos(x:i64):bool{<x>0};
f=sh(x:i64):str{<s.concat("n";"\(x)")};
f=inc(x:i64):i64{<x+1};
f=main():i64{
  let xs=@(0-3;7;0);
  io.println(j.enc(xs.map(&pos)));
  let bs=xs.map(&pos);
  io.println("b1=\(bs.get(1))");
  io.println(j.enc(xs.map(&sh)));
  io.println("s1=\(xs.map(&sh).get(1))");
  io.println(j.enc(xs.map(&inc)));
  io.println("i0=\(xs.map(&inc).get(0))");
  <0
};
TOKE

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  exp_b=$'[false,true,false]\nb1=true'
  exp_s=$'["n-3","n7","n0"]\ns1=n7'
  exp_i=$'[-2,8,1]\ni0=-2'
  got_b=$(echo "$out" | sed -n 1,2p); got_s=$(echo "$out" | sed -n 3,4p); got_i=$(echo "$out" | sed -n 5,6p)
  [ "$rc" -eq 0 ] || fail "exit $rc"
  [ "$got_b" = "$exp_b" ] && pass "bool map is @bool" || fail "bool: $(echo "$got_b" | tr '\n' '|')"
  [ "$got_s" = "$exp_s" ] && pass "str map is @str" || fail "str: $(echo "$got_s" | tr '\n' '|')"
  [ "$got_i" = "$exp_i" ] && pass "i64 map unchanged" || fail "i64: $(echo "$got_i" | tr '\n' '|')"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2301: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
