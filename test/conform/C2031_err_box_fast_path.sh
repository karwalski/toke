#!/usr/bin/env bash
# C2031_err_box_fast_path.sh — the std error-box raise keeps its contract on
# the 127.492 fast path (story 127.492).
#
# THE CHANGE: tk_err_box (src/stdlib/tk_runtime.c), which every std raise
# calls, zeroed the box with a variable-size memset: a libc call plus a
# heavier prologue on every raise.  A box of at most TK_ERR_INLINE (64) bytes
# is now zeroed with a constant 64-byte store, no call.  Measured on Darwin
# arm64 (cli-argv form a, `mt args.get(1){$ok:v v;$err:e "default"}` on the
# miss path, PAT_N=16,384,000, -O2): 0.20 s -> 0.12-0.13 s user CPU; form b
# (the count check) 0.06 s.  The bound is the buffer's capacity, which is
# never below 64 once the buffer exists, so zeroing past nbytes is safe; this
# script pins the contract the fast path must keep.
#
# WHAT IS ASSERTED (tk_runtime.c built -O2, and again under ASan+UBSan):
#   1. every size 1..64, after the box was filled with 0xA5: the first
#      nbytes come back zero, the pointer is 8-byte aligned and never NULL
#   2. the same pointer (one reused buffer) for every size <= 64
#   3. a wide box (200 bytes) after a narrow one: zeroed, then a narrow box
#      after the wide one is zeroed too (the grown buffer still serves it)
#   4. nbytes 0 and negative: a zeroed 16-byte box, as before
#   5. after tk_err_release (the atexit hook) the next box is valid and zeroed
#   6. end to end: `mt args.get(1){$ok:v v;$err:e e.msg}` on a miss prints
#      the message and the default, a let-bound error keeps its message
#      across later raises (127.103), and a 1,000,000-miss loop runs clean
#
# Negative control: replace the fast path's `memset(eb->buf, 0, TK_ERR_INLINE)`
# with nothing -> case 1 fails ("not zeroed").
#
# Story: 127.492
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C2031_STDLIB:-${REPO_ROOT}/src/stdlib}"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2031_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
rc=0

echo "C2031: error-box fast path (127.492)"
echo "------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tk_runtime.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

static int zero(const void *p, int64_t n) {
    const unsigned char *b = p;
    for (int64_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

int main(void) {
    int ok_zero = 1, ok_align = 1, ok_same = 1;
    void *first = NULL;
    for (int64_t n = 1; n <= 64; n++) {
        unsigned char *b = tk_err_box(64);
        memset(b, 0xA5, 64);                 /* a previous raise's payload */
        b = tk_err_box(n);
        if (!b || !zero(b, n)) ok_zero = 0;
        if (((uintptr_t)b & 7u) != 0) ok_align = 0;
        if (!first) first = b; else if (b != first) ok_same = 0;
    }
    CHECK(ok_zero,  "1. sizes 1..64 after a dirty box: the first nbytes are zeroed, never NULL");
    CHECK(ok_align, "1. the box is 8-byte aligned at every size");
    CHECK(ok_same,  "2. one reused buffer for every size <= 64");

    unsigned char *w = tk_err_box(200);
    int wide = w && zero(w, 200);
    memset(w, 0x5A, 200);
    unsigned char *n8 = tk_err_box(8);
    CHECK(wide, "3. a 200-byte box after narrow ones: zeroed");
    CHECK(n8 && zero(n8, 8), "3. a narrow box after the buffer grew: zeroed");
    memset(n8, 0x5A, 64);
    unsigned char *w2 = tk_err_box(200);
    CHECK(w2 && zero(w2, 200), "3. a wide box again after a dirty narrow one: zeroed");

    memset(tk_err_box(64), 0x77, 64);
    unsigned char *z0 = tk_err_box(0), *zneg;
    int ok0 = z0 && zero(z0, 16);
    memset(tk_err_box(64), 0x77, 64);
    zneg = tk_err_box(-5);
    CHECK(ok0 && zneg && zero(zneg, 16), "4. nbytes 0 and -5: a zeroed 16-byte box");

    tk_err_release();
    unsigned char *r = tk_err_box(16);
    CHECK(r && zero(r, 16), "5. after tk_err_release the next box is valid and zeroed");
    memset(r, 1, 16);
    r = tk_err_box(16);
    CHECK(r && zero(r, 16), "5. ... and the fast path serves the re-created buffer");

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

build_run() {   # <label> <flags>
    if ! "${CC}" -std=c99 -D_GNU_SOURCE $2 -iquote "${S}" -o "${WORK}/t_$1" "${WORK}/t.c" \
            "${S}/tk_runtime.c" "${S}/capabilities.c" "${S}/args.c" "${S}/str.c" \
            >"${WORK}/cc_$1.log" 2>&1; then
        echo "  FAIL: build ($1): $(head -c 600 "${WORK}/cc_$1.log")"; rc=1; return
    fi
    echo "  [$1]"
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t_$1" || rc=1
}
build_run O2 "-O2"
build_run asan "-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

# 6. end to end through the compiler
if [ ! -x "${TKC}" ]; then echo "  FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
cat > "${WORK}/e.tk" <<'TK'
m=main;
i=io:std.io;
i=args:std.args;
i=env:std.env;
f=main():i64{
  io.println(mt args.get(1){$ok:v v;$err:e e.msg});
  io.println(mt args.get(1){$ok:v v;$err:e "default"});
  let k=args.get(5);
  let n=env.getint("C2031_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+(mt args.get(1){$ok:v v.len;$err:e e.msg.len})};
  io.println(mt k{$ok:v v;$err:e e.msg});
  io.println("t=\(t)");
  <0
};
TK
if "${TKC}" -O2 --allow-all -o "${WORK}/e" "${WORK}/e.tk" >"${WORK}/e.log" 2>&1; then
    got="$(C2031_N=1000000 "${WORK}/e" 2>&1)"
    want="$(printf 'args.get: index out of bounds\ndefault\nargs.get: index out of bounds\nt=29000000')"
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: 6. args.get misses: message, default, let-bound error kept across 1,000,000 later raises"
    else
        echo "  FAIL: 6. end to end"; printf '      expected: %s\n      got:      %s\n' "${want}" "${got}"; rc=1
    fi
else
    echo "  FAIL: 6. tkc build: $(head -c 600 "${WORK}/e.log")"; rc=1
fi
echo ""
[ "${rc}" -eq 0 ] && echo "C2031: PASS" || echo "C2031: FAIL (exit ${rc})"
exit "${rc}"
