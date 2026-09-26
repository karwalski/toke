#!/usr/bin/env bash
# C127_unsigned_codegen.sh — u8/u16/u32/u64 lower with unsigned semantics
# (story 127.217).
#
# THE DEFECT.  The unsigned kinds share the signed kinds' IR widths (u8 and i8
# are both `i8`) and codegen never asked the checker which it had: a u8 of 200
# printed -56 and compared below 100, a u32 of 3e9 compared below 1e9, u64
# addition trapped RT002 on crossing 2^63, and `u8 as str` handed an i8 to an
# i64 parameter (invalid IR, E9003).  Codegen now reads the checker's type:
# icmp u*/udiv/urem/lshr/u*.with.overflow when both operands are unsigned (or
# one is and the other a literal), zext when widening an unsigned value, and a
# u64 >= 2^63 renders as (v udiv 10)(v urem 10).
#
# WHAT THIS DRIVES.  Outside string interpolation only: comparison, `/`, `%`,
# `>>`, u64 add past 2^63, `as str` and `as i64` from each width, at -O0 and
# -O2, plus u64 overflow past 2^64, which must still trap RT002.  Inside
# `\(...)` the operands are not yet typed by the checker (reported to the
# type-checker batch); test/codegen/cg_unsigned.tk stays quarantined for that.
# Every expected value is derived by hand in the comments below.
#
# Negative control: make binop_is_unsigned() return 0 — the comparisons, the
# u64 add and the u64 div/rem/shift checks fail.
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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c127_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C127: unsigned integer codegen"
echo "------------------------------"

cat > "${WORK}/u.tk" <<'EOF'
m=c127;
i=io:std.io;
f=ck(n:str;ok:bool):i64{
  if(ok){io.println("ok \(n)")}el{io.println("FAIL \(n)")};
  <0
};
f=main():i64{
  let e:u8=200 as u8;
  let small:u8=100 as u8;
  ck("u8 gt";e>small);
  ck("u8 ge";e>=small);
  ck("u8 lt";small<e);
  let g:u16=60000 as u16;
  let h:u16=1000 as u16;
  ck("u16 gt";g>h);
  let p:u32=3000000000 as u32;
  let q:u32=1000000000 as u32;
  ck("u32 gt";p>q);
  let a:u64=9000000000000000000 as u64;
  let s:u64=a+a;
  ck("u64 add past 2^63";s>a);
  io.println(s as str);
  io.println(e as str);
  io.println(g as str);
  io.println(p as str);
  let w:i64=e as i64;
  io.println(w as str);
  let d:u64=s/(7 as u64);
  io.println(d as str);
  let r:u64=s%(7 as u64);
  io.println(r as str);
  let sh:u64=s>>(60 as u64);
  io.println(sh as str);
  <0
};
EOF
# 200+… : s = 18000000000000000000 (< 2^64 = 18446744073709551616);
# s/7 = 2571428571428571428 (7*that = 17999999999999999996), s%7 = 4;
# 2^60 = 1152921504606846976, 15*2^60 = 17293822569102704640 <= s < 16*2^60,
# so s>>60 = 15.
cat > "${WORK}/u.expected" <<'EOF'
ok u8 gt
ok u8 ge
ok u8 lt
ok u16 gt
ok u32 gt
ok u64 add past 2^63
18000000000000000000
200
60000
3000000000
200
2571428571428571428
4
15
EOF
cat > "${WORK}/ov.tk" <<'EOF'
m=c127ov;
i=io:std.io;
f=main():i64{
  let a:u64=9000000000000000000 as u64;
  let s:u64=a+a+a;
  io.println("FAIL u64 add past 2^64 wrapped");
  <0
};
EOF

for o in -O0 -O2; do
    if "${TKC}" --allow-all "${o}" --out "${WORK}/u" "${WORK}/u.tk" >"${WORK}/u.b" 2>&1; then
        "${WORK}/u" >"${WORK}/u.out" 2>&1 </dev/null; rc=$?
        if [ "${rc}" = 0 ] && cmp -s "${WORK}/u.out" "${WORK}/u.expected"; then
            pass "unsigned operations ${o}"
        else
            fail "unsigned operations ${o}: exit ${rc}; diff:"; diff "${WORK}/u.expected" "${WORK}/u.out" | head -12
        fi
    else
        fail "unsigned operations ${o}: did not build: $(awk '/"severity":"error"/' "${WORK}/u.b" | head -1 | cut -c1-220)"
    fi
    if "${TKC}" --allow-all "${o}" --out "${WORK}/ov" "${WORK}/ov.tk" >"${WORK}/ov.b" 2>&1; then
        "${WORK}/ov" >"${WORK}/ov.out" 2>"${WORK}/ov.err" </dev/null; rc=$?
        if [ "${rc}" = 1 ] && [ ! -s "${WORK}/ov.out" ] && awk 'index($0,"RT002:")==1{f=1} END{exit !f}' "${WORK}/ov.err"; then
            pass "u64 add past 2^64 traps RT002 ${o}"
        else
            fail "u64 add past 2^64 ${o}: exit ${rc}, stdout [$(cat "${WORK}/ov.out")], stderr [$(head -1 "${WORK}/ov.err")]"
        fi
    else
        fail "u64 overflow ${o}: did not build"
    fi
done

echo ""
echo "C127: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
