#!/usr/bin/env bash
# C811_append_struct_alloc_bound.sh — bytes allocated by an append loop whose
# accumulator ends up in a struct literal grow LINEARLY with the number of
# appends (127.307).
#
# The measure is deterministic: a small malloc/calloc/realloc interposer
# (DYLD_INSERT_LIBRARIES on Darwin, LD_PRELOAD on glibc Linux) sums the bytes
# requested and writes the total at exit.  It does not depend on wall-clock
# time or on what else the machine is doing, so it is stable under load.
#
# Before 127.307 the struct cases copied the whole block on every append:
# at N=6000 f64 appends that is ~144 MB requested (8*N^2/2) and a 6000/1500
# ratio of ~16; in place it is well under 1 MB and a ratio near 4.
#
# For each shape (returned struct literal, top-level `let r=$s{..};<r`, and the
# `<o` control) the test asserts: bytes(6000) < 1 MiB and
# bytes(6000)/bytes(1500) < 8.  If the interposer cannot be built or loaded
# on this host the measurement is skipped by name (CONFORM-SKIP).
#
# Story: 127.307

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc not found at ${TKC}"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/c811.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

CC_="$(command -v clang || command -v cc || true)"
OS="$(uname -s)"
cat > "${WORK}/shim.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
static unsigned long long g_bytes;
#ifdef __APPLE__
#define INTERPOSE(r, o) __attribute__((used)) static struct { const void *a; const void *b; } \
    ip_##o __attribute__((section("__DATA,__interpose"))) = { (const void *)&r, (const void *)&o };
static void *my_malloc(size_t n) { g_bytes += n; return malloc(n); }
static void *my_calloc(size_t a, size_t b) { g_bytes += a * b; return calloc(a, b); }
static void *my_realloc(void *p, size_t n) { g_bytes += n; return realloc(p, n); }
INTERPOSE(my_malloc, malloc)
INTERPOSE(my_calloc, calloc)
INTERPOSE(my_realloc, realloc)
#else
extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
void *malloc(size_t n) { g_bytes += n; return __libc_malloc(n); }
void *calloc(size_t a, size_t b) { g_bytes += a * b; return __libc_calloc(a, b); }
void *realloc(void *p, size_t n) { g_bytes += n; return __libc_realloc(p, n); }
#endif
__attribute__((destructor)) static void report(void) {
    const char *f = getenv("C811_OUT");
    FILE *o = f ? fopen(f, "w") : NULL;
    if (o) { fprintf(o, "%llu\n", g_bytes); fclose(o); }
}
EOF
case "${OS}" in
    Darwin) SHIM="${WORK}/shim.dylib"; SHFLAGS="-dynamiclib"; PRE=DYLD_INSERT_LIBRARIES ;;
    *)      SHIM="${WORK}/shim.so";    SHFLAGS="-shared -fPIC"; PRE=LD_PRELOAD ;;
esac
SHIM_OK=0
if [ -n "${CC_}" ] && "${CC_}" ${SHFLAGS} -O1 -o "${SHIM}" "${WORK}/shim.c" > "${WORK}/shim.log" 2>&1; then
    SHIM_OK=1
fi

# bytes <shape> <N>  ->  bytes requested by the whole program run, or ""
bytes() {
    local shape="$1" n="$2" d="${WORK}/$1_$2" tail
    mkdir -p "$d"
    case "$shape" in
        retlit) tail='<$s{d:o;n:n}' ;;
        stored) tail='let r=$s{d:o;n:o.len};
  <r' ;;
        direct) tail='<$s{d:o;n:n}' ;;
    esac
    if [ "$shape" = direct ]; then
        cat > "$d/p.tk" <<EOF
m=main;
i=io:std.io;
t=\$s{d:@f64;n:i64};
f=build(n:i64):@f64{
  let o:@f64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i as f64)};
  <o
};
f=main():i64{let s=\$s{d:build(${n});n:${n}};io.println("\(s.d.len as \$str)");<0};
EOF
    else
        cat > "$d/p.tk" <<EOF
m=main;
i=io:std.io;
t=\$s{d:@f64;n:i64};
f=build(n:i64):\$s{
  let o:@f64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i as f64)};
  ${tail}
};
f=main():i64{let s=build(${n});io.println("\(s.d.len as \$str)");<0};
EOF
    fi
    "${TKC}" "$d/p.tk" -o "$d/p" > "$d/build.log" 2>&1 || { echo "build-failed"; return; }
    local out; out="$(env C811_OUT="$d/bytes" "${PRE}=${SHIM}" "$d/p" 2>&1)"
    [ "$out" = "$n" ] || { echo "bad-output:${out}"; return; }
    cat "$d/bytes" 2>/dev/null
}

if [ "${SHIM_OK}" -ne 1 ]; then
    echo "CONFORM-SKIP: allocation interposer could not be built ($(head -c 200 "${WORK}/shim.log"))"
else
    # The interposer must actually observe allocations, or every bound below
    # would pass vacuously.
    probe="$(bytes direct 1500)"
    case "$probe" in
        ''|*[!0-9]*)
            echo "CONFORM-SKIP: allocation interposer not loaded on this host (${PRE}; got '${probe}')" ;;
        *)
            if [ "$probe" -le 0 ]; then
                echo "CONFORM-SKIP: allocation interposer counted 0 bytes (${PRE} not honoured)"
            else
                for shape in retlit stored direct; do
                    a="$(bytes "$shape" 1500)"; b="$(bytes "$shape" 6000)"
                    case "$a$b" in
                        ''|*[!0-9]*) bad "${shape}: could not measure (1500=${a} 6000=${b})"; continue ;;
                    esac
                    if [ "$a" -le 0 ]; then bad "${shape}: 0 bytes at N=1500"; continue; fi
                    if [ "$b" -lt 1048576 ] && [ $((b)) -lt $((a * 8)) ]; then
                        ok "${shape}: bytes requested N=1500 ${a}, N=6000 ${b} (< 1 MiB and < 8x: linear)"
                    else
                        bad "${shape}: bytes requested N=1500 ${a}, N=6000 ${b} (want < 1 MiB and < 8x; quadratic copying is ~144 MB, ~16x)"
                    fi
                done
            fi ;;
    esac
fi

echo "C811: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
