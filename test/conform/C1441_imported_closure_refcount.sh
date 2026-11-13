#!/usr/bin/env bash
# C1441_imported_closure_refcount.sh -- a closure returned by a function of
# another user module is owned by the caller's binding and released with it,
# as a closure returned by a function of the same module is (memory-model
# §2.5; stories 124.0c item 3, 127.377).
#
# THE DEFECT: the importer typed `l.mkadd(i)` as nothing (127.377), so
# `let a=l.mkadd(i); a(1)` printed the argument and the environment was never
# released: 2,000,000 iterations peaked at ~64 MB.  With the result typed as
# a function value, the binding must also treat the call's result as OWNED
# (the defining module's return already hands the caller a reference): a
# retain at the binding would leak one reference per iteration.
#
#   1. RSS flat and the right sum over 2,000,000 iterations, -O0 and -O2.
#      Per iteration a(1)+l.ap(a;2) = (1+i)+(2+i), so the total is
#      n(n-1) + 3n = 4000004000000.  Bound 20 MB (~1.5 MB here).
#   2. No use-after-free under a scribbling allocator: the closure is
#      called through the binding, passed back into its module, copied and
#      re-bound in a loop.
#
# Negative controls: TKC built from fix/z161-mt-iface-partial (6dca2454) --
# 1 red at both levels (2000005000000, ~64 MB peak), 2 red (wrong values).
# fnv_owned without its 127.377 import branch (retain at the binding) --
# 1 red at both levels (peak far above the bound); 2 green.
#
# Story: 127.377

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
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
cat > "${WORK}/a_lib.tk" <<'EOF'
m=c1441.lib;
f=mkadd(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};
f=ap(h:(i64):i64;x:i64):i64{<h(x)};
f=keep(h:(i64):i64):(i64):i64{<h};
EOF
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
[ -f "${WORK}/c1441.lib.tki" ] || echo "  FAIL: setup: --emit-interface produced no c1441.lib.tki"

cat > "${WORK}/p1.tk" <<'EOF'
m=c1441.main;
i=io:std.io;
i=l:c1441.lib;
f=main():i64{
  let s=mut.0;
  lp(let i=0;i<2000000;i=i+1){
    let a=l.mkadd(i);
    s=s+a(1)+l.ap(a;2);
  };
  io.println("\(s)");
  <0};
EOF
for o in -O0 -O2; do
    rm -f "${WORK}/b1"
    if ( cd "${WORK}" && "${TKC}" "${o}" a_lib.tk p1.tk -o b1 >/dev/null 2>&1 ); then
        out="$("${WORK}/b1" | tr -d '\n')"
        kb="$(maxrss_kb "${WORK}/b1")"
        if [ "${out}" = "4000004000000" ] && [ -n "${kb}" ] && [ "${kb}" -lt 20480 ]; then
            echo "PASS 1-rss-flat ${o} (${kb} KiB peak)"; PASS=$((PASS + 1))
        else
            echo "FAIL 1-rss-flat ${o}"; echo "        out=${out} peak=${kb:-unmeasured} KiB (bound 20480)"; FAIL=$((FAIL + 1))
        fi
    else echo "FAIL 1-rss-flat ${o} (build failed)"; FAIL=$((FAIL + 1)); fi
done

cat > "${WORK}/p2.tk" <<'EOF'
m=c1441.main;
i=io:std.io;
i=l:c1441.lib;
f=main():i64{
  let a=l.mkadd(3);
  let b=a;
  let k=l.keep(l.mkadd(10));
  let v=mut.l.mkadd(0);
  lp(let i=0;i<50;i=i+1){ let t=l.mkadd(i); v=t };
  io.println("\(a(1)) \(b(2)) \(k(1)) \(v(0)) \(l.ap(a;4)) \(l.ap(l.mkadd(7);1))");
  <0};
EOF
# a(1)=4; b(2)=5; k(1)=11; v=mkadd(49): v(0)=49; ap(a;4)=7; ap(mkadd(7);1)=8
for o in -O0 -O2; do
    rm -f "${WORK}/b2"
    if ( cd "${WORK}" && "${TKC}" "${o}" a_lib.tk p2.tk -o b2 >/dev/null 2>&1 ); then
        expect "2-no-uaf ${o}" "$(scribble_run "${WORK}/b2")" "4 5 11 49 7 8 rc=0"
    else echo "FAIL 2-no-uaf ${o} (build failed)"; FAIL=$((FAIL + 1)); fi
done

echo "C1441: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
