#!/usr/bin/env bash
# C224_recursive_nonretaining_callee.sh — passing a linear array to a
# RECURSIVE user function that only reads it keeps the array on the in-place
# append path; a recursive function that hands the array back still demotes
# it to copying (story 127.171).
#
# THE DEFECT. 127.108 decides from the callee's own body whether a parameter
# is retained, but cut recursion off at a fixed depth and answered "retains":
# `f=sumfrom(a:@i64;i:i64):i64{..<a.get(i)+sumfrom(a;i+1)}` demoted every
# `acc=acc.append(i)` of a caller to tk_array_append_w — O(N²) time and live
# memory (N=40,000: 3.19 GB peak on this host).  A query that meets itself
# round a cycle is now answered on the assumption under test (the greatest
# fixpoint), which every other use on the cycle still has to justify
# (llvm.c la_param_nonretaining).
#
# Negative control: TKC built from main at c8575da — cases 1 and 2 red
# ("0 1" and "0 1": no in-place call).  Case 3 is green on both; it was shown
# red ("2 0") by injecting an unconditional "does not retain" answer into
# la_param_nonretaining.
#
# Story: 127.171

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

# 1. direct recursion, read-only
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=sumfrom(a:@i64;i:i64):i64{
  if(i>=a.len){<0};
  <a.get(i)+sumfrom(a;i+1)
};
f=main():i64{
  let acc=mut.@();
  let tot=mut.0;
  lp(let i=0;i<5;i=i+1){ acc=acc.append(i); tot=tot+sumfrom(acc;0) };
  io.println("\(acc.len) \(tot)");
  <0
};
EOF
expect "1-direct-recursion-lowering" "$(lowering "${WORK}/p1.tk")" "1 0"
expect "1-direct-recursion-runs"     "$(runs "${WORK}/p1.tk")" "5 20 "

# 2. mutual recursion, read-only
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=ev(a:@i64;i:i64):i64{ if(i>=a.len){<0}; <a.get(i)+od(a;i+1) };
f=od(a:@i64;i:i64):i64{ if(i>=a.len){<0}; <ev(a;i+1) };
f=main():i64{
  let acc=mut.@();
  let tot=mut.0;
  lp(let i=0;i<4;i=i+1){ acc=acc.append(i+1); tot=tot+ev(acc;0) };
  io.println("\(acc.len) \(tot)");
  <0
};
EOF
expect "2-mutual-recursion-lowering" "$(lowering "${WORK}/p2.tk")" "1 0"
expect "2-mutual-recursion-runs"     "$(runs "${WORK}/p2.tk")" "4 10 "

# 3. recursion that hands the array back retains it: copying, and the
#    returned alias must not see later appends
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
f=keep(a:@i64;i:i64):@i64{ if(i>=a.len){<a}; <keep(a;i+1) };
f=main():i64{
  let b=mut.@(0);
  b=b.append(1);
  let k=keep(b;0);
  b=b.append(2);
  io.println("\(b.len) \(k.len)");
  <0
};
EOF
expect "3-retaining-recursion-lowering" "$(lowering "${WORK}/p3.tk")" "0 2"
expect "3-retaining-recursion-runs"     "$(runs "${WORK}/p3.tk")" "3 2 "

echo "C224: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
