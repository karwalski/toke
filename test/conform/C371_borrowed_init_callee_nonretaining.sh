#!/usr/bin/env bash
# C371_borrowed_init_callee_nonretaining.sh — a callee that binds its array
# parameter with `let r=mut.p`, where r is itself a linear array, does not
# retain p: the caller's accumulator stays in place (127.237 (2), Batch GB2).
#
# THE DEFECT.  127.108 decides whether a user callee retains an argument
# from the callee's own body (la_param_nonretaining).  `let result=mut.arr`
# names the parameter as a bare initialiser, which la_bad_use counted as a
# retention — but when `result` is linear in the callee, that binding is a
# borrowed initialiser copied once (127.229 (7)), so `result` never holds
# the caller's block.  Eval 0221 (`sorthelper(evens)`): every
# `evens=evens+@(elem)` in `sort` copied the accumulator (n=2000: see the
# perf note in the batch report).  llvm.c: la_bad_use's bind case now asks
# la_local_linear_in, which runs the same linear-array analysis the callee
# gets when it is emitted.
#
# Each case checks the LOWERING in the caller as "<in-place> <copying>"
# calls and the program's OUTPUT.
#
# Negative control: TKC built from main at 9f901bf — case 1 red ("0 1": the
# append copies).  Retaining controls 2-4 are green on both: the callee's
# `r` is not linear there (never updated and returned; bound twice; captured
# by a closure), so r does alias the parameter and the caller must copy.
# Injection (la_local_linear_in answering 1 always) -> 2, 3 and 4 red on
# the lowering ("2 0": the caller appended in place over a block the callee
# still aliases); their outputs stayed green only because the append
# happened to reallocate.
#
# Story: 127.237 (2)

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
# lowering FILE FN: "<in-place> <copying>" append calls in FN's body
lowering() {
    "${TKC}" --emit-llvm "$1" -o "${WORK}/p.ll" >/dev/null 2>&1 || { echo "compile-failed"; return; }
    awk -v fn="$2" '$0 ~ "^define .*@" fn "\\(" {f=1}
        f&&/call i64 @tk_array_append_inplace_w/{i++}
        f&&/call i64 @tk_array_append_w\(/{c++}
        f&&/call i8\* @tk_array_concat\(i8\* %t[0-9]+, i8\* %t/{c++}
        f&&/^}/{f=0} END{printf "%d %d", i, c}' "${WORK}/p.ll"
}

# 1. Eval 0221's shape: the callee's r is linear (borrowed init + .set).
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=bump(arr:@i64):@i64{
  let n=arr.len;
  let r=mut.arr;
  lp(let i=0;i<n;i=i+1){r=r.set(i;r.get(i)+100)};
  <r
};
f=build():i64{
  let acc=mut.@();
  lp(let i=0;i<4;i=i+1){acc=acc+@(i)};
  let b=bump(acc);
  io.println("\(acc.get(0)) \(acc.get(3)) \(b.get(0)) \(b.get(3))");
  <0
};
f=main():i64{<build()};
EOF
expect "1-linear-callee-lowering" "$(lowering "${WORK}/p1.tk" t_build)" "1 0"
expect "1-linear-callee-runs"     "$(runs "${WORK}/p1.tk")" "0 3 100 103 "

# 2. Control: the callee's r is never updated and is returned — r IS the
#    caller's block, so the caller's later append must not show in b.
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=same(arr:@i64):@i64{ let r=mut.arr; <r };
f=build():i64{
  let acc=mut.@();
  acc=acc+@(1);
  let b=same(acc);
  acc=acc+@(2);
  io.println("\(acc.len) \(b.len)");
  <0
};
f=main():i64{<build()};
EOF
expect "2-alias-callee-lowering" "$(lowering "${WORK}/p2.tk" t_build)" "0 2"
expect "2-alias-callee-runs"     "$(runs "${WORK}/p2.tk")" "2 1 "

# 3. Control: r bound twice in the callee (the second binding is the
#    parameter again, after an update) — not linear, so r may alias arr.
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
f=twice(arr:@i64):@i64{
  let r=mut.arr;
  r=r.set(0;50);
  let r=mut.arr;
  <r
};
f=build():i64{
  let acc=mut.@();
  acc=acc+@(1);
  let b=twice(acc);
  acc=acc+@(2);
  io.println("\(acc.len) \(b.len)");
  <0
};
f=main():i64{<build()};
EOF
expect "3-rebound-callee-lowering" "$(lowering "${WORK}/p3.tk" t_build)" "0 2"
expect "3-rebound-callee-runs"     "$(runs "${WORK}/p3.tk")" "2 1 "

# 4. Control: the callee's r is captured by a closure — not linear.
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;
f=cap(arr:@i64):i64{
  let r=mut.arr;
  r=r.set(0;9);
  let g=fn():i64{<r.len};
  <g()
};
f=build():i64{
  let acc=mut.@();
  acc=acc+@(1);
  let k=cap(acc);
  acc=acc+@(2);
  io.println("\(acc.len) \(k)");
  <0
};
f=main():i64{<build()};
EOF
expect "4-captured-callee-lowering" "$(lowering "${WORK}/p4.tk" t_build)" "0 2"
expect "4-captured-callee-runs"     "$(runs "${WORK}/p4.tk")" "2 1 "

echo "C371: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
