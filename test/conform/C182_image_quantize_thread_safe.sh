#!/usr/bin/env bash
# C182_image_quantize_thread_safe.sh — image_quantize keeps no shared state
# (story 135.19 item 5).
#
# THE DEFECT.  The median-cut sort passed its key channel to qsort's
# comparator through a static global (`g_sort_ch`), so two threads quantizing
# at once raced on it and could sort a bucket by the other thread's channel.
# It is now a counting sort on the one-byte key with no static state.
#
# The harness quantizes two images whose widest channel differs (red for one,
# blue for the other) on two threads, many times, under ThreadSanitizer, and
# compares every concurrent result with the serial one.  Section 2 asserts the
# structural property directly: no writable static in image.o.
#
# Negative control: src/stdlib/image.c from main at 7c16729 — section 1
# "WARNING: ThreadSanitizer: data race" on g_sort_ch; section 2 lists
# `_g_sort_ch` in image.o's writable data.
#
# Story: 135.19

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMAGE_C="${IMAGE_C:-${REPO_ROOT}/src/stdlib/image.c}"
CC="${CC:-cc}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/h.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "image.h"
#define W 32
#define H 32
static TkImgBuf mk(int wide_ch) {
    TkImgBuf b; memset(&b, 0, sizeof b);
    b.width = W; b.height = H; b.channels = 3;
    b.data = malloc(W * H * 3); b.data_len = W * H * 3;
    for (int i = 0; i < W * H; i++)
        for (int c = 0; c < 3; c++)
            b.data[i * 3 + c] = (uint8_t)(c == wide_ch ? (i * 37) & 255 : 100 + (i & 3));
    return b;
}
static TkImgBuf A, B; static uint8_t *ra, *rb; static int bad[2];
static void *run(void *p) {
    int which = (int)(intptr_t)p;
    for (int k = 0; k < 200; k++) {
        TkImgBuf o = image_quantize(which ? B : A, 8);
        if (!o.data || memcmp(o.data, which ? rb : ra, W * H * 3)) bad[which] = 1;
        free(o.data);
    }
    return NULL;
}
int main(void) {
    A = mk(0); B = mk(2);
    TkImgBuf s1 = image_quantize(A, 8), s2 = image_quantize(B, 8);
    ra = s1.data; rb = s2.data;
    if (!ra || !rb || !memcmp(ra, rb, W * H * 3)) { printf("BAD-FIXTURE\n"); return 0; }
    pthread_t t1, t2;
    pthread_create(&t1, NULL, run, (void *)0);
    pthread_create(&t2, NULL, run, (void *)1);
    pthread_join(t1, NULL); pthread_join(t2, NULL);
    printf("%s\n", (bad[0] || bad[1]) ? "MISMATCH" : "same");
    return 0;
}
EOF

# 1. concurrent quantize under ThreadSanitizer
if "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 -fsanitize=thread -iquote "$(dirname "${IMAGE_C}")" \
       -o "${WORK}/h" "${WORK}/h.c" "${IMAGE_C}" -lm -lz -lpthread >"${WORK}/cc.log" 2>&1; then
    out="$(TSAN_OPTIONS=halt_on_error=0 "${WORK}/h" 2>&1)"
    expect "1a-no-data-race"      "$(printf '%s\n' "${out}" | grep -c 'ThreadSanitizer: data race')" "0"
    expect "1b-concurrent-equals-serial" "$(printf '%s\n' "${out}" | grep -c '^same$')" "1"
else
    echo "SKIP 1: this host cannot build with -fsanitize=thread"; sed -n 1,5p "${WORK}/cc.log"
fi

# 2. no writable static data in image.o
"${CC}" -std=c99 -D_GNU_SOURCE -O2 -iquote "$(dirname "${IMAGE_C}")" -c -o "${WORK}/image.o" "${IMAGE_C}" 2>/dev/null
expect "2-no-writable-static" "$(nm "${WORK}/image.o" | awk '$2 ~ /^[bBdDsS]$/ && $3 ~ /sort/' | wc -l | tr -d ' ')" "0"

echo "C182: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
