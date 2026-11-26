#!/usr/bin/env bash
# C2303_map_chain_and_fallback_tag.sh — a typed `.map` result flows through
# the next receiver method, and the emitter's fallback tag (for a receiver
# the checker does not establish) finds the mapped function (story 127.550).
#
# THE DEFECT: `fs.map(&dbl).filter(&big)`, `.get(1)` and `.map(&dbl)` on a
# map result carried the wrong "@i64" on to the next call; a field receiver
# (`h.xs.map(&dbl)`) or a `mut` binding reassigned by a map, which the
# checker does not type, reached llvm.c expr_struct_type's 127.44 tag, whose
# lookup_fn(`dbl`) missed the symbol `p_dbl`.
#
# THE FIX: the checker's @f64 is the receiver type of the chained call; the
# fallback tag resolves `&f` through fvc_ref_sig (mangled) and spells bool
# and record returns too ("@bool" is honoured by json_arr_shape's tag
# fallback, as "@f64" and "@str" were).
#
# WHAT THIS CHECKS (runs): map->filter, ->len, ->get, ->reduce (i64
# accumulator over f64 elements), ->map; field receiver f64 and bool; a mut
# binding `m=m.map(&dbl)`.
#
# Negative control: main 58bd74e8 prints bit patterns for the f64 lines and
# [0,1] for the bool field map (len and reduce are already right).
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2303_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2303: chained .map results and the fallback tag (127.550)"
echo "----------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=j:std.json;
t=$hold{xs:@f64;n:i64};
f=dbl(x:f64):f64{<x*2.0};
f=big(x:f64):bool{<x>4.0};
f=gt2(x:f64):bool{<x>2.0};
f=cnt(a:i64;x:f64):i64{if(x>4.0){<a+1};<a};
f=main():i64{
  let fs=@(1.5;2.5;0.25);
  io.println(j.enc(fs.map(&dbl).filter(&big)));
  io.println("len=\(fs.map(&dbl).len())");
  io.println("g1=\(fs.map(&dbl).get(1))");
  io.println("n=\(fs.map(&dbl).reduce(0;&cnt))");
  io.println(j.enc(fs.map(&dbl).map(&dbl)));
  let h=$hold{xs:@(1.5;2.5);n:1};
  io.println(j.enc(h.xs.map(&dbl)));
  io.println(j.enc(h.xs.map(&gt2)));
  let m=mut.fs.map(&dbl);
  m=m.map(&dbl);
  io.println(j.enc(m));
  <0
};
TOKE

if "${TKC}" --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  exp_c=$'[5]\nlen=3\ng1=5\nn=1\n[6,10,1]'
  exp_f=$'[3,5]\n[false,true]\n[6,10,1]'
  got_c=$(echo "$out" | sed -n 1,5p); got_f=$(echo "$out" | sed -n 6,8p)
  [ "$rc" -eq 0 ] || fail "exit $rc"
  [ "$got_c" = "$exp_c" ] && pass "map chained into filter/len/get/reduce/map" || fail "chain: $(echo "$got_c" | tr '\n' '|')"
  [ "$got_f" = "$exp_f" ] && pass "fallback tag: field receiver f64/bool, mut binding" || fail "fallback: $(echo "$got_f" | tr '\n' '|')"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2303: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
