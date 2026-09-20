#!/usr/bin/env bash
# C014_map_value_semantics.sh — `m.set(k;v)` returns a NEW map and leaves the
# receiver alone (story 127.55).
#
# `tk_map_set_w` used to call tk_map_put on the receiver and return the same
# handle, so `let m2 = m.set(k;v)` ALSO changed `m`.  Every binding derived
# from a map aliased it: in a chain `m -> m2 -> m3` all three were one object,
# and `m` could be observed to contain a key added two statements after it was
# bound.  Nothing diagnosed it.  `docs/spec/semantics.md` gives maps the same
# value semantics as arrays, and arrays have honoured them since 114.18, so
# this was a core guarantee failing silently.
#
# The fix mirrors the arrangement arrays already had (ADR-0006 D2):
#   tk_map_set_w          copies  — the general case
#   tk_map_set_inplace_w  mutates — emitted ONLY at a self-update
#                                   `m = m.set(k;v)` where the per-function
#                                   linearity analysis proves m is unaliased
# so value semantics are restored without making the documented accumulator
# idiom O(N^2).
#
# Case 1 is the assertion the 127.46 test deliberately did not make.  Case 3 is
# the one that must never regress: when the map IS aliased the fast path must
# not fire.  Case 4 pins the performance half — a test that only proved
# correctness would pass against a fix that made every `m = m.set(...)` copy.
#
# Stories: 127.55, 127.46

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

WORK="$(mktemp -d /tmp/tkc_mapvalue_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C014: map .set has value semantics"
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

# ── Case 1: the defect itself ────────────────────────────────────────────
# Before 127.55 this printed "m.len=2 m2.len=2".
run_case "set leaves the receiver alone" "m.len=1 m2.len=2" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1);
  let m2=m.set("b";2);
  io.println("m.len=\(m.len) m2.len=\(m2.len)");
  <0
};'

# ── Case 2: a chain of derived maps are distinct objects ─────────────────
# Before the fix this printed "m=3 m2=3 m3=3" and "m has c=1" — the original
# map reported a key added two statements later.
run_case "a derivation chain does not alias" "m=1 m2=2 m3=3
m has c=0" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1);
  let m2=m.set("b";2);
  let m3=m2.set("c";3);
  io.println("m=\(m.len) m2=\(m2.len) m3=\(m3.len)");
  io.println("m has c=\(m.contains("c"))");
  <0
};'

# ── Case 3: an ALIASED map must not take the in-place path ───────────────
# `let keep=m` creates a durable second reference, so the linearity analysis
# must refuse to mutate m in place.  If the fast path ever fires here, keep
# sees the later write and this prints "keep.len=2".
run_case "an aliased receiver keeps the copying path" "keep.len=1 m.len=2" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=mut.@("a":1);
  let keep=m;
  m=m.set("b";2);
  io.println("keep.len=\(keep.len) m.len=\(m.len)");
  <0
};'

# ── Case 4: the accumulator idiom is still correct ───────────────────────
run_case "the accumulator idiom accumulates" "acc len=6 k3=3" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=mut.@("seed":0);
  lp(let i=0;i<5;i=i+1){
    m=m.set("k\(i)";i)
  };
  io.println("acc len=\(m.len) k3=\(m.get("k3"))");
  <0
};'

# ── Case 5: …and it is still the IN-PLACE lowering ───────────────────────
# The performance half.  Without this, a fix that simply made every set copy
# would pass cases 1-4 while turning every map accumulation into O(N^2).
printf '%s\n' 'm=main;
i=io:std.io;
f=main():i64{
  let m=mut.@("seed":0);
  lp(let i=0;i<5;i=i+1){
    m=m.set("k\(i)";i)
  };
  io.println("len=\(m.len)");
  <0
};' > acc.tk
if "${TKC}" --allow-all --emit-llvm --out acc.ll acc.tk >/dev/null 2>&1 &&
   grep -q 'call i64 @tk_map_set_inplace_w' acc.ll; then
    echo "  PASS: a linearly-owned accumulator lowers to tk_map_set_inplace_w"
    PASS=$((PASS + 1))
else
    echo "  FAIL: the accumulator idiom did NOT take the in-place path (O(N^2) regression)"
    FAIL=$((FAIL + 1))
fi

# ── Case 6: a non-self-update set lowers to the COPYING variant ──────────
printf '%s\n' 'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1);
  let m2=m.set("b";2);
  io.println("\(m.len) \(m2.len)");
  <0
};' > cpy.tk
if "${TKC}" --allow-all --emit-llvm --out cpy.ll cpy.tk >/dev/null 2>&1 &&
   grep -q 'call i64 @tk_map_set_w' cpy.ll &&
   ! grep -q 'call i64 @tk_map_set_inplace_w' cpy.ll; then
    echo "  PASS: a let-bound m.set lowers to the copying tk_map_set_w"
    PASS=$((PASS + 1))
else
    echo "  FAIL: a non-self-update set did not use the copying variant"
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
