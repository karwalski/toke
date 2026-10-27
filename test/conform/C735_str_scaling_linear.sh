#!/usr/bin/env bash
# C735_str_scaling_linear.sh — string scanning is linear (story 127.170,
# gate 2 of docs/runtime-abi.md §3.1).
#
# THE DEFECT.  A toke string carried no length, so every per-call operation
# that needs one rescanned the whole string: `.len` is strlen, `t.charat(k)`
# bounds-checks with strlen, and str.indexfrom proves `start` is inside the
# string with a memchr of the prefix.  The ordinary lexer loop
#     lp(let k=0;k<t.len;k=k+1){ if(t.charat(k)==44){...} }
# was quadratic twice over: 160 G instructions at 64,000 tokens.
#
# THE FIX.  Converted producers (interpolation, concat, slice, the builder's
# build, ...) return SEALED strings that carry their length in a header
# tk_strlen reads in O(1) (src/stdlib/tk_strhdr.h).
#
# Asserted: test/perf/strlen/scaling.sh — the 127.91 scanner, the `.len`
# loop and the `.len` + charat loop at N and 4N; each must grow by at most 6x
# in INSTRUCTIONS RETIRED (linear ~4, quadratic ~16; load cannot move a
# count).  Control, in this script: the same `.len` + charat loop over a
# string from a producer that is NOT converted (str.frombytes, whose result is
# malloc'd by the [byte] unpack) must still be quadratic -- charat, not `.len`
# alone, because since step 4 (C737) a loop-invariant `.len` is hoisted and is
# linear even on an unsealed string — so the gate is seen to fail on
# this host, with this counter.  If str.frombytes is ever sealed, pick another
# unconverted producer (scripts/check_str_strlen.py --census lists them).
#
# Negative control: TOKE=<tkc built from main fb0592a1> — all three FAIL
# (ratios 9.72, 17.02, 17.75).
#
# Counter: macOS /usr/bin/time -l; Linux perf stat.  Neither: CONFORM-SKIP.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
[ -x "${TKC}" ] || { echo "FAIL: tkc not found at ${TKC}"; exit 1; }
PASS=0; FAIL=0

echo "C735: string scanning is linear (127.170 gate 2)"
echo "--------------------------------------------------"

out="$(TOKE="${TKC}" sh "${REPO_ROOT}/test/perf/strlen/scaling.sh" 2>&1)"; rc=$?
if [ "${rc}" -eq 77 ]; then
    echo "CONFORM-SKIP: $(echo "${out}" | sed -n 's/^SKIP: *//p' | head -1)"
    exit 0
fi
while IFS= read -r line; do
    case "${line}" in
        PASS*) echo "  ${line}"; PASS=$((PASS + 1)) ;;
        FAIL*) echo "  ${line}"; FAIL=$((FAIL + 1)) ;;
    esac
done <<< "${out}"
[ "${PASS}" -eq 3 ] || { echo "  FAIL: expected 3 PASS lines from scaling.sh, got ${PASS} (rc ${rc})"; FAIL=$((FAIL + 1)); }

# Control: an unconverted producer keeps the old, quadratic behaviour.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c735_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/perf/strlen"
cat > "${WORK}/perf/strlen/len_loop.tk" <<'EOF'
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let b=s.builder();
  lp(let i=0;i<n;i=i+1){s.add(b;"tok\(i),")};
  let t=s.frombytes(s.bytes(s.build(b)));
  let cnt=mut.0;
  lp(let k=0;k<t.len;k=k+1){
    if(t.charat(k)==44){cnt=cnt+1}
  };
  io.println("n=\(cnt)");
  <0
};
EOF
cp "${REPO_ROOT}/test/perf/strlen/scaling.sh" "${WORK}/perf/strlen/"
ctl="$(PROGS=len_loop TOKE="${TKC}" sh "${WORK}/perf/strlen/scaling.sh" 2>&1)"
if echo "${ctl}" | grep -q '^FAIL len_loop: .*(quadratic)'; then
    echo "  PASS: control: the charat loop over an unconverted producer is still quadratic — $(echo "${ctl}" | sed -n 's/^FAIL len_loop: //p')"
    PASS=$((PASS + 1))
else
    echo "  FAIL: control did not fail the gate:"; echo "${ctl}" | sed 's/^/      /'
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
