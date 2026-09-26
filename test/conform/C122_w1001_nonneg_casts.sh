#!/usr/bin/env bash
# C122_w1001_nonneg_casts.sh — W1001 "lossy cast from 'i64' to 'u64'" is not
# raised where the i64 is provably non-negative (stories 127.189, 127.204).
#
# THE NOISE. W1001 fired on every `x as u64` with an i64 `x`:
#   * a literal — `args.get(1 as u64)` (8 in ooke main.tk, 127.204).  The
#     grammar has no negative literal (`-1` is unary minus over `1`), so a
#     NODE_INT_LIT is never negative;
#   * a loop counter `lp(let i=0;i<n;i=i+1){..i as u64..}` (155 corpus
#     records after 127.178, 127.189) — it starts at a literal >= 0, its step
#     adds a literal, and nothing else writes it.
#
# WHAT IS ASSERTED. Those two shapes are clean; everything that is not proven
# keeps the warning: a counter the body also assigns, a counter whose step
# is not `+ literal`, an expression over the counter, a counter shadowed by a
# parameter of the same name, and a computed value.
#
# Negative control: TKC built from main at 8f4a843 — cases 1 and 2 draw W1001
# (2 of 7 red); a mutant without the parameter/arm-binding guard in
# cast_src_nonneg loses case 6's warning (1 of 7 red).
#
# Stories: 127.189, 127.204

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# the W1001 count in one --check
w1001() { printf 'm=t;i=io:std.io;\n%s\n' "$1" > "${WORK}/p.tk"
          "${TKC}" --check "${WORK}/p.tk" 2>&1 | awk '/"error_code":"W1001"/{n++} END{print n+0}'; }

expect "1-literal-clean" \
  "$(w1001 'f=main():i64{let a=@(1;2;3);let x=a.get(1 as u64);io.println("\(x)");<0};')" "0"
expect "2-loop-counter-clean" \
  "$(w1001 'f=main():i64{let a=@(1;2;3);lp(let i=0;i<3;i=i+1){io.println("\(a.get(i as u64))");};<0};')" "0"
expect "3-counter-also-assigned-warns" \
  "$(w1001 'f=main():i64{lp(let j=0;j<3;j=j+1){j=j-2;let z=j as u64;};<0};')" "1"
expect "4-counter-multiplied-warns" \
  "$(w1001 'f=main():i64{lp(let k=1;k<9;k=k*2){let z=k as u64;};<0};')" "1"
expect "5-expression-over-counter-warns" \
  "$(w1001 'f=main():i64{lp(let q=0;q<3;q=q+1){let z=(q-1) as u64;};<0};')" "1"
expect "6-param-of-same-name-warns" \
  "$(w1001 'f=g(i:i64):u64{lp(let i=0;i<1;i=i+1){};<i as u64};f=main():i64{<0};')" "1"
expect "7-computed-value-warns" \
  "$(w1001 'f=g(n:i64):u64{<(n-1) as u64};f=main():i64{<0};')" "1"

echo "C122: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
