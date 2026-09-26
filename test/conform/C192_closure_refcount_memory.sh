#!/usr/bin/env bash
# C192_closure_refcount_memory.sh — closure environments are reference
# counted: released when the last reference goes, never while one is live
# (memory-model §2.5; story 124.0c item 3).
#
# THE DEFECT, on main at 7c16729: an environment was freed only when a
# closure literal was applied on the spot, or when a `let` provably held the
# sole reference (Batch CC, c6c101a).  A closure that was copied, passed,
# returned or captured leaked its malloc'd environment once per execution:
# program 1 below peaks at 378,016 KiB at -O0 over 3,000,000 iterations on
# main and prints -13499998500000 at both levels (the wrong sum, C191);
# program 2 does not build on main (E9003) and program 3 prints 0.
#
# NOW: each environment carries a count (and a mask of the captures that are
# themselves closures).  A binding of a function value owns a slot released
# on re-bind, on assignment and at every return; returns, copies and
# captures retain; call arguments are borrowed and an owned temporary is
# released after the call (ownership notes above fnv_is_local in llvm.c).
#
#   1. RSS flat: 3,000,000 iterations that bind, copy, pass, return, capture
#      and compose closures, at -O0 and -O2.  Bound 20 MB (it runs at
#      ~1.6 MB here); main exceeds it tenfold.
#   2. No use-after-free under a scribbling allocator (GuardMalloc on macOS,
#      MALLOC_PERTURB_ on glibc): every escaping shape, where a premature
#      release would free an environment that is still called.
#   3. A chain of 200,000 closures each capturing the previous one is freed
#      by one release at return — iteratively, not one stack frame per link
#      — and a 1,000-link chain still computes through every link first.
#
# Negative controls (each injected in llvm.c, rebuilt, then reverted;
# re-verified by Batch EBc):
#   * emit_owned never retains -> 2 and 3 exit 139 under GuardMalloc (a
#     copied closure's environment is freed by the first binding's slot),
#     and 1 crashes too: all 5 red.  Under ASan the same build reports
#     heap-use-after-free.
#   * the slot release at a `let` re-bind is dropped -> 1 fails its bound
#     at -O0 and -O2 (~472,000 KiB peak: the loop's environments are never
#     freed); 2 and 3 stay green.
#
# Story: 124.0c (item 3)

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
maxrss_kb() {   # BIN -> peak RSS in KiB, or empty
    if [ "$(uname)" = Darwin ]; then
        /usr/bin/time -l "$1" 2>&1 >/dev/null | awk '/maximum resident set size/{print int($1/1024)}'
    else
        /usr/bin/time -v "$1" 2>&1 >/dev/null | awk -F: '/Maximum resident set size/{gsub(/ /,"",$2); print $2}'
    fi
}
scribble_run() {   # BIN -> stdout (one line) + rc, under a scribbling allocator
    if [ "$(uname)" = Darwin ] && [ -e /usr/lib/libgmalloc.dylib ]; then
        DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib "$1" 2>/dev/null | tr '\n' ' '
        echo "rc=${PIPESTATUS[0]}"
    else
        MALLOC_PERTURB_=85 MALLOC_CHECK_=3 "$1" 2>/dev/null | tr '\n' ' '
        echo "rc=${PIPESTATUS[0]}"
    fi
}

# 1. RSS over 3,000,000 iterations.  Per iteration the sum gains
#    ap(w;0) + i + i - 3i - 1 = (2i+1) + 2i - 3i - 1 = i, so the total is
#    n(n-1)/2 = 4499998500000.
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=ap(g:(i64):i64;x:i64):i64{<g(x)};
f=mk(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
f=keep(g:(i64):i64):(i64):i64{<g};
f=compose(f1:(i64):i64;f2:(i64):i64):(i64):i64{<fn(x:i64):i64{<f2(f1(x))}};
f=main():i64{
  let s=mut.0;
  lp(let i=0;i<3000000;i=i+1){
    let t=mk(i);
    let u=t;
    let k=keep(u);
    let c=compose(k;mk(1));
    let w=fn(x:i64):i64{<c(x)+t(x)};
    s=s+ap(w;0)+ap(fn(x:i64):i64{<x+i};0)+ap(mk(i);0)-3*i-1;
  };
  io.println("\(s)");
  <0
};
EOF
for o in -O0 -O2; do
    if "${TKC}" "${o}" "${WORK}/p1.tk" -o "${WORK}/b1" >/dev/null 2>&1; then
        out="$("${WORK}/b1" | tr -d '\n')"
        kb="$(maxrss_kb "${WORK}/b1")"
        if [ "${out}" = "4499998500000" ] && [ -n "${kb}" ] && [ "${kb}" -lt 20480 ]; then
            echo "PASS 1-rss-flat ${o} (${kb} KiB peak)"; PASS=$((PASS + 1))
        else
            echo "FAIL 1-rss-flat ${o}"; echo "        out=${out} peak=${kb:-unmeasured} KiB (bound 20480)"; FAIL=$((FAIL + 1))
        fi
    else echo "FAIL 1-rss-flat ${o} (build failed)"; FAIL=$((FAIL + 1)); fi
done

# 2. every escaping shape, under a scribbling allocator
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
t=$e{$bad:i64};
f=ap(g:(i64):i64;x:i64):i64{<g(x)};
f=mk(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
f=keep(g:(i64):i64):(i64):i64{<g};
f=copyret(n:i64):(i64):i64{let g=fn(x:i64):i64{<x*n};let h=g;<h};
f=capret(n:i64):(i64):i64{let g=mk(n);<fn(x:i64):i64{<g(x)*2}};
f=compose(f1:(i64):i64;f2:(i64):i64):(i64):i64{<fn(x:i64):i64{<f2(f1(x))}};
f=fall(n:i64):(i64):i64!$e{let g=mk(n);if(n>5){<$e{$bad:g(0)}};<g};
f=main():i64{
  let a=copyret(3);
  let b=capret(4);
  let c=compose(a;b);
  let d=keep(mk(5));
  let e=mt fall(2) {$ok:v v; $err:x mk(0-1)};
  let f2=mt fall(9) {$ok:v v; $err:x mk(0-2)};
  let v=mut.mk(0);
  lp(let i=0;i<50;i=i+1){ let t=mk(i); v=t };
  let arr=@(mk(1);keep(a));
  let z=arr.get(1);
  io.println("\(a(1)) \(b(1)) \(c(1)) \(d(1)) \(e(1)) \(f2(1)) \(v(0)) \(z(2)) \(ap(c;2))");
  <0
};
EOF
# a(1)=1*3=3; b(1)=(1+4)*2=10; c(1)=b(a(1))=b(3)=(3+4)*2=14; d(1)=6;
# e=fall(2) ok -> mk(2): e(1)=3; f2: fall(9) errs -> mk(-2): f2(1)=-1;
# v=mk(49): v(0)=49; z=keep(a)=a: z(2)=6; ap(c;2)=b(a(2))=b(6)=20.
for o in -O0 -O2; do
    if "${TKC}" "${o}" "${WORK}/p2.tk" -o "${WORK}/b2" >/dev/null 2>&1; then
        expect "2-escaping-shapes-no-uaf ${o}" "$(scribble_run "${WORK}/b2")" "3 10 14 6 3 -1 49 6 20 rc=0"
    else echo "FAIL 2-escaping-shapes-no-uaf ${o} (build failed)"; FAIL=$((FAIL + 1)); fi
done

# 3. a long capture chain: 200,000 links freed at return, then a 1,000-link
#    chain called through every link (1000 + 0).
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
f=chain(n:i64):(i64):i64{
  let cur=mut.fn(x:i64):i64{<x};
  lp(let i=0;i<n;i=i+1){ let prev=cur; cur=fn(x:i64):i64{<prev(x)+1} };
  <cur
};
f=main():i64{
  let big=chain(200000);
  let small=chain(1000);
  io.println("\(small(0))");
  <0
};
EOF
if "${TKC}" -O0 "${WORK}/p3.tk" -o "${WORK}/b3" >/dev/null 2>&1; then
    expect "3-capture-chain-release" "$(scribble_run "${WORK}/b3")" "1000 rc=0"
else echo "FAIL 3-capture-chain-release (build failed)"; FAIL=$((FAIL + 1)); fi

echo "C192: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
