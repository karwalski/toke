#!/usr/bin/env bash
# C221_str_producer_inplace_append.sh — an array accumulator seeded by a
# std.str producer (`s.bytes`, `s.split`, `s.fields`, `s.chars`) takes the
# in-place append, exactly as one seeded by a literal does (story 135.20
# item 2).
#
# THE DEFECT. The 114.18 linear-array analysis accepted only an array or map
# literal (or a self-update) as a FRESH binding, so
#   let big=mut.s.bytes(""); lp(...){ big=big.append(v) }
# lowered every append to the copying tk_array_append_w: O(N²) time and,
# since the discarded blocks are never freed, O(N²) live memory — 20,000
# appends peaked at 1.29 GB on this host (test/perf/perfmeasure.sh).  Every
# std.str export returning an array allocates its block with tk_arr_alloc and
# keeps no reference to it, and std.str takes no array it could hand back,
# so its result is as fresh as a literal (llvm.c la_fresh_std_call).
#
# Case 3 guards value semantics: an accumulator that is ALSO aliased must
# still copy, and the alias must not see the appends.
#
# Negative control: TKC built from main at c8575da — cases 1 and 2 red ("0 1":
# no in-place call, one copying call); case 4 red (rss=824999936).
#
# Story: 135.20

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
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
# "<in-place calls> <copying calls>" in main's body
lowering() {
    "${TKC}" --emit-llvm "$1" -o "${WORK}/p.ll" >/dev/null 2>&1 || { echo "compile-failed"; return; }
    awk '/^define .*@tk_main\(/{f=1} f&&/call i64 @tk_array_append_inplace_w/{i++} f&&/call i64 @tk_array_append_w/{c++} f&&/^}/{f=0} END{printf "%d %d", i, c}' "${WORK}/p.ll"
}

# 1. the row's program: a [byte] accumulator seeded by s.bytes
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let big=mut.s.bytes("ab");
  lp(let i=0;i<5;i=i+1){ big=big.append(i+60) };
  io.println("\(big.len) \(big.get(0)) \(big.get(6))");
  <0
};
EOF
expect "1-bytes-lowering" "$(lowering "${WORK}/p1.tk")" "1 0"
expect "1-bytes-runs"     "$(runs "${WORK}/p1.tk")" "7 97 64 "

# 2. a [str] accumulator seeded by s.split
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let parts=mut.s.split("a,b";",");
  lp(let i=0;i<3;i=i+1){ parts=parts.append("x") };
  io.println("\(parts.len) \(parts.get(1)) \(parts.get(4))");
  <0
};
EOF
expect "2-split-lowering" "$(lowering "${WORK}/p2.tk")" "1 0"
expect "2-split-runs"     "$(runs "${WORK}/p2.tk")" "5 b x "

# 3. aliased: must stay on the copying path, and the alias must not change
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let big=mut.s.bytes("ab");
  let keep=big;
  big=big.append(1);
  io.println("\(big.len) \(keep.len)");
  <0
};
EOF
expect "3-aliased-lowering" "$(lowering "${WORK}/p3.tk")" "0 1"
expect "3-aliased-runs"     "$(runs "${WORK}/p3.tk")" "3 2 "

# 4. 20,000 appends stay small (Darwin: ru_maxrss in bytes)
if [ "$(uname -s)" = "Darwin" ]; then
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let big=mut.s.bytes("");
  lp(let i=0;i<20000;i=i+1){ big=big.append(i%256) };
  io.println("\(big.len)");
  <0
};
EOF
    if "${TKC}" "${WORK}/p4.tk" -o "${WORK}/b4" >/dev/null 2>&1; then
        rss=$( { /usr/bin/time -l "${WORK}/b4" >/dev/null; } 2>&1 | awk '/maximum resident/{print $1}')
        if [ -n "${rss}" ] && [ "${rss}" -lt 104857600 ]; then r=under-100MB; else r="rss=${rss}"; fi
    else r=compile-failed; fi
    expect "4-20000-appends-rss" "${r}" "under-100MB"
else
    echo "SKIP 4-20000-appends-rss (ru_maxrss units are host-specific; Darwin only)"
fi

echo "C221: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
