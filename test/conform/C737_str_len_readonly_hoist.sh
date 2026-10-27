#!/usr/bin/env bash
# C737_str_len_readonly_hoist.sh — `.len` and charat are declared read-only,
# so LLVM hoists a loop-invariant `.len` (story 127.170, landing step 4 of
# docs/runtime-abi.md §3.1, option S0).
#
# THE GAP.  `t.len` on a $str lowers to a call of tk_str_len_w (charat to
# tk_str_charat_w), declared with no attributes, so LLVM had to assume the
# call writes memory: `lp(let k=0;k<t.len;k=k+1){...}` called it on every
# iteration even when nothing in the loop could change t.
#
# THE CHANGE.  The four pure length readers (tk_str_len, tk_str_len_w,
# tk_str_char_at, tk_str_charat_w) are declared `nounwind willreturn
# readonly` in the compiler's declaration table (src/stdlib_tables.c).
#
# Asserted:
#   1. the emitted IR declares tk_str_len_w and tk_str_charat_w with the
#      attributes, and still calls them (no inlined runtime layout);
#   2. hoisting, by count: a loop of N iterations whose body reads the
#      .len of a loop-invariant string costs under 12 instructions per
#      iteration at -O2 (instructions retired at N and 4N; load cannot move
#      a count).  Without the attributes it is a call per iteration (~30).
#      (A charat AFTER an overflow-checked add is not hoisted: the trap
#      branch before it means it is not guaranteed to execute -- the
#      reason runtime-abi calls S0 a complement, not a fix);
#   3. no stale length: a loop that REBINDS the string each iteration and
#      tests its .len stops at the right length, and charat reads the new
#      string — the attribute licenses hoisting only what does not change.
#
# Negative control: TKC built from main fb0592a1 — case 1 fails (plain
# declares) and case 2 fails (a call per iteration); case 3 passes
# both ways.
#
# Counter for case 2: macOS /usr/bin/time -l; Linux perf stat; neither ->
# that case is reported as CONFORM-SKIP-worthy and skipped (the others run).

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
[ -x "${TKC}" ] || { echo "FAIL: tkc not found at ${TKC}"; exit 1; }
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c737_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "      expected: $3"; echo "      got:      $2"; FAIL=$((FAIL + 1)); fi
}

echo "C737: .len / charat declared readonly; loop-invariant .len hoisted (127.170 S0)"
echo "--------------------------------------------------"

cat > "${WORK}/h.tk" <<'EOF'
m=main;
i=io:std.io;
i=env:std.env;
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t="hello \(n)!!";
  let tot=mut.0;
  lp(let k=0;k<n;k=k+1){tot=tot+t.len};
  io.println("t=\(tot) \(t.charat(0))");
  <0
};
EOF

# 1. declarations
if "${TKC}" --allow-all --emit-llvm "${WORK}/h.tk" --out "${WORK}/h.ll" >/dev/null 2>&1; then
    expect "declare tk_str_len_w nounwind willreturn readonly" \
        "$(grep -c '^declare i64 @tk_str_len_w(i64) nounwind willreturn readonly$' "${WORK}/h.ll")" "1"
    expect "declare tk_str_charat_w nounwind willreturn readonly" \
        "$(grep -c '^declare i64 @tk_str_charat_w(i64, i64) nounwind willreturn readonly$' "${WORK}/h.ll")" "1"
    expect ".len is still a call of tk_str_len_w" \
        "$(grep -c 'call i64 @tk_str_len_w' "${WORK}/h.ll" | awk '{print ($1>0)?"yes":"no"}')" "yes"
else
    echo "  FAIL: --emit-llvm"; FAIL=$((FAIL + 1))
fi

# 2. hoisting, by instructions retired per iteration
if [ "$(uname -s)" = Darwin ] && [ -x /usr/bin/time ]; then
    insns() { PAT_N=$2 /usr/bin/time -l "$1" 2>&1 >/dev/null | awk '/instructions retired/{print $1}'; }
elif command -v perf >/dev/null 2>&1 && perf stat -x, -e instructions:u true >/dev/null 2>&1; then
    insns() { PAT_N=$2 perf stat -x, -e instructions:u "$1" 2>&1 >/dev/null | awk -F, '/instructions/{print $1}'; }
else
    insns() { echo ""; }
fi
if "${TKC}" -O2 --allow-all "${WORK}/h.tk" --out "${WORK}/h" >/dev/null 2>&1; then
    expect "output" "$(PAT_N=1000 "${WORK}/h")" "t=12000 104"
    a=$(insns "${WORK}/h" 250000); b=$(insns "${WORK}/h" 1000000)
    case "$a$b" in
        ''|*[!0-9]*) echo "  SKIP: no instruction counter on this host (case 2 not run)" ;;
        *) per=$(awk -v a="$a" -v b="$b" 'BEGIN{printf "%.1f", (b-a)/750000}')
           v=$(awk -v p="$per" 'BEGIN{print (p<12)?"hoisted":"per-iteration call"}')
           expect "loop-invariant .len: ${per} instructions/iteration (< 12)" "${v}" "hoisted" ;;
    esac
else
    echo "  FAIL: -O2 compile"; FAIL=$((FAIL + 1))
fi

# 3. no stale length when the string changes inside the loop
cat > "${WORK}/r.tk" <<'EOF'
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let t=mut."ab";
  let steps=mut.0;
  lp(let k=0;t.len<10;k=k+1){t=s.concat(t;"x");steps=steps+1};
  let u=mut."q";
  let last=mut.0;
  lp(let j=0;j<3;j=j+1){u="\(j)\(u)";last=last+u.charat(0)};
  io.println("\(t.len) \(steps) \(t) \(u) \(last)");
  <0
};
EOF
if "${TKC}" -O2 --allow-all "${WORK}/r.tk" --out "${WORK}/r" >/dev/null 2>&1; then
    expect "rebinding inside the loop: lengths and bytes follow the new string" \
        "$("${WORK}/r")" "10 8 abxxxxxxxx 210q 147"
else
    echo "  FAIL: -O2 compile (rebinding)"; FAIL=$((FAIL + 1))
fi

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
