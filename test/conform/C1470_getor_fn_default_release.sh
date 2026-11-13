#!/usr/bin/env bash
# C1470_getor_fn_default_release.sh -- a function value passed as the
# default of `m.getor(k;d)` on a map of function values is released: the
# call yields a reference its consumer owns on both the hit and the miss
# path, so neither the unused default nor the returned one leaks, and the
# default that IS returned is never freed under its user (story 127.383).
#
# THE DEFECT: the receiver dispatch emitted the default as an ordinary
# argument -- a closure literal is +1 and a function-valued local took a
# reference there -- and nothing released it after tk_map_getor_w, while
# the binding retained the result as if borrowed from the map.  One env
# leaked per call: 1,000,000 iterations of
# `let h=u.getor("a";fn(x:i64):i64{<x+j})` peaked at ~33 MB (~1.5 MB with
# a named function as the default).
#
# NOW: the default is emitted borrowed, the result is retained, and an owned
# default is released after the call (map_getor_fnv / fnv_owned in llvm.c).
#
#   1. RSS flat and the right sum over 1,000,000 iterations, -O0 and -O2:
#      a hit (closure default unused), a miss (closure default returned), and
#      a function-valued local as the default of both.  Bound 20 MB.
#   2. No use-after-free / double free, under AddressSanitizer (when clang
#      can link it) and under a scribbling allocator: hit, miss, a local
#      default, a let mut re-bind, a getor returned from a function, and a
#      getor passed straight into a function.
#
# Negative controls: TKC built from fix/z169-user-args-getor-fn (87153202) --
# 1 red at both levels (~96 MB peak, sums right), 2 green.  This fix without
# the retain of the result -- 1 and 2 red (no output / SIGSEGV under the
# scribbling allocator; ASan heap-use-after-free).
#
# Story: 127.383

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

# 1: hit, miss, local default -- per iteration h(1)+g(1)+e(1)+f(1)
#    = 11 + (1+j) + (1+2j) + 11, so the total is 24n + 3*n(n-1)/2
#    = 24000000 + 1499998500000 = 1500022500000 for n = 1,000,000.
cat > "${WORK}/p1.tk" <<'EOF'
m=c1470.p1;
i=io:std.io;
f=main():i64{
  let k=10;
  let u=@("a":fn(x:i64):i64{<x+k});
  let s=mut.0;
  lp(let j=0;j<1000000;j=j+1){
    let h=u.getor("a";fn(x:i64):i64{<x+j});
    let g=u.getor("z";fn(x:i64):i64{<x+j});
    let d=fn(x:i64):i64{<x+j+j};
    let e=u.getor("z";d);
    let f=u.getor("a";d);
    s=s+h(1)+g(1)+e(1)+f(1)
  };
  io.println("\(s)");
  <0};
EOF
for o in -O0 -O2; do
    rm -f "${WORK}/b1"
    if ( cd "${WORK}" && "${TKC}" "${o}" p1.tk -o b1 >/dev/null 2>&1 ); then
        out="$("${WORK}/b1" | tr -d '\n')"
        kb="$(maxrss_kb "${WORK}/b1")"
        if [ "${out}" = "1500022500000" ] && [ -n "${kb}" ] && [ "${kb}" -lt 20480 ]; then
            echo "PASS 1-rss-flat ${o} (${kb} KiB peak)"; PASS=$((PASS + 1))
        else
            echo "FAIL 1-rss-flat ${o}"; echo "        out=${out} peak=${kb:-unmeasured} KiB (bound 20480)"; FAIL=$((FAIL + 1))
        fi
    else echo "FAIL 1-rss-flat ${o} (build failed)"; FAIL=$((FAIL + 1)); fi
done

# 2: every ownership path.  Per iteration
#    h(1)=11, g(1)=1+j, e(1)=j, d(1)=j, w(1)=0, p(1)=1+j, q(1)=11, ap(..)=2
#    = 26 + 4j, so over 2000 iterations 52000 + 4*1999000 = 8048000.
cat > "${WORK}/p2.tk" <<'EOF'
m=c1470.p2;
i=io:std.io;
f=ap(h:(i64):i64;x:i64):i64{<h(x)};
f=pick(u:@(str:(i64):i64);k:str;j:i64):(i64):i64{<u.getor(k;fn(x:i64):i64{<x+j})};
f=main():i64{
  let k=10;
  let u=@("a":fn(x:i64):i64{<x+k});
  let s=mut.0;
  lp(let j=0;j<2000;j=j+1){
    let h=u.getor("a";fn(x:i64):i64{<x+j});
    let g=u.getor("z";fn(x:i64):i64{<x+j});
    let d=fn(x:i64):i64{<x*j};
    let e=u.getor("z";d);
    let w=mut.u.getor("a";d);
    w=u.getor("z";fn(x:i64):i64{<x-1});
    let p=pick(u;"z";j);
    let q=pick(u;"a";j);
    s=s+h(1)+g(1)+e(1)+d(1)+w(1)+p(1)+q(1)+ap(u.getor("z";fn(x:i64):i64{<x+1});1)
  };
  io.println("\(s)");
  <0};
EOF
for o in -O0 -O2; do
    rm -f "${WORK}/b2"
    if ( cd "${WORK}" && "${TKC}" "${o}" p2.tk -o b2 >/dev/null 2>&1 ); then
        expect "2-no-uaf scribble ${o}" "$(scribble_run "${WORK}/b2")" "8048000 rc=0"
    else echo "FAIL 2-no-uaf scribble ${o} (build failed)"; FAIL=$((FAIL + 1)); fi
done

# 2 under AddressSanitizer: tkc builds with clang (TKC_CC), so a wrapper that
# adds -fsanitize=address instruments the program and intercepts free.
CLANG="$(command -v clang || true)"
if [ -n "${CLANG}" ]; then
    printf '#!/bin/sh\nexec "%s" "$@" -fsanitize=address -fno-omit-frame-pointer\n' "${CLANG}" > "${WORK}/asancc"
    chmod +x "${WORK}/asancc"
    rm -f "${WORK}/b2a"
    if ( cd "${WORK}" && TKC_CC="${WORK}/asancc" "${TKC}" p2.tk -o b2a >/dev/null 2>&1 ) && [ -x "${WORK}/b2a" ]; then
        a="$(ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 "${WORK}/b2a" 2>"${WORK}/asan.err" | tr -d '\n'; echo " rc=${PIPESTATUS[0]}")"
        n="$(grep -c 'ERROR: AddressSanitizer' "${WORK}/asan.err")"
        expect "2-no-uaf asan" "${a} asan_errors=${n}" "8048000 rc=0 asan_errors=0"
    else
        echo "SKIP 2-no-uaf asan (clang cannot build with -fsanitize=address here)"
    fi
else
    echo "SKIP 2-no-uaf asan (no clang on PATH)"
fi

echo "C1470: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
