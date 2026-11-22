#!/usr/bin/env bash
# C2030_tiff_bomb_guard.sh — std.image refuses a TIFF whose strips cannot
# back its declared page, before allocating it (story 127.488).
#
# THE DEFECT (measured before 127.488, -O2 standalone fuzz_image): a 1,686-
# byte little-endian TIFF found by libFuzzer as a slow unit
# (test/fuzz/corpus-adversarial/image_tiff_short_strip_127488.tif) declares
# a 2600 x 65435 one-bit page, uncompressed, in one strip of 1,590 bytes.
# Every TIFF decompressor zero-fills what a strip does not cover, so it
# decoded to a 170 MB page of zeros in 0.4 s, and the fuzz harness's encode /
# re-decode / flip of it took the run to 3.2 s user CPU and ~1 GB peak RSS.
# Any service decoding untrusted TIFFs paid that per 1.7 KB request.
#
# THE FIX: each codec has a ceiling on what n input bytes can decode to
# (none 1:1, PackBits 128 per 2 bytes, LZW 4096 bytes per 9-bit code,
# Deflate 1032:1, CCITT one row per bit).  Declared strip bytes beyond that
# ceiling ("unbacked") may total at most 1 MiB per page; past that the page
# is refused before any pixel buffer is allocated.
#
# WHAT THIS DRIVES (image.c built -O2, plus an ASan+UBSan build):
#   - the slow unit: refused with the guard's message, < 0.1 s CPU, peak RSS
#     grows < 8 MiB; the fuzz harness replays it in < 1 s CPU
#   - a 4096 x 4096 8-bit page declared over a 64-byte uncompressed strip:
#     refused
#   - controls that must still decode exactly: the tracked TIFF seeds
#     (CCITT MH/G3/G4, two-page), a full 2600 x 2000 uncompressed page, a
#     4096 x 4096 all-zero Deflate page compressed by zlib level 9 (near
#     DEFLATE's ratio ceiling, so the bound is not too tight), a 4096 x 256
#     PackBits page coded at PackBits' ceiling (128 bytes per 2), and a
#     small page whose short final strip is zero-filled (unbacked < 1 MiB,
#     behaviour unchanged).
#
# Negative control: C2030_STDLIB=<dir> builds against another stdlib tree;
# the pre-fix tree (`git archive b1333d8c src/stdlib`) fails the two refusal
# checks, the CPU and RSS bounds and the harness replay (3.7 s CPU); every
# control passes there too.
#
# Story: 127.488

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C2030_STDLIB:-${REPO_ROOT}/src/stdlib}"
SEEDS="${REPO_ROOT}/test/fuzz/seeds/image"
SLOW="${REPO_ROOT}/test/fuzz/corpus-adversarial/image_tiff_short_strip_127488.tif"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2030_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C2030: TIFF decompression-bomb guard (127.488)"
echo "----------------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <zlib.h>
#include "image.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

static long long rss_bytes(void) {
    struct rusage u; getrusage(RUSAGE_SELF, &u);
#ifdef __APPLE__
    return (long long)u.ru_maxrss;
#else
    return (long long)u.ru_maxrss * 1024;
#endif
}
static double cpu_s(void) {
    struct rusage u; getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6
         + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) { *n = 0; return NULL; }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1); *n = fread(b, 1, (size_t)l, f); fclose(f);
    return b;
}

/* One-strip little-endian TIFF: w x h, bps, spp 1, BlackIsZero, `comp`,
 * strip = data[0..dn). */
static uint8_t *mktiff(uint32_t w, uint32_t h, uint16_t bps, uint16_t comp,
                       const uint8_t *data, size_t dn, size_t *len) {
    uint8_t *t = calloc(1, dn + 256); size_t p = 8;
    memcpy(t, "II*\0", 4);
    memcpy(t + p, data, dn); uint32_t so = (uint32_t)p; p += dn; if (p & 1) p++;
    uint32_t ifd = (uint32_t)p; t[4]=ifd; t[5]=ifd>>8; t[6]=ifd>>16; t[7]=ifd>>24;
    struct { uint16_t tag, ty; uint32_t v; } e[] = {
        {256,4,w},{257,4,h},{258,3,bps},{259,3,comp},{262,3,1},
        {273,4,so},{277,3,1},{278,4,h},{279,4,(uint32_t)dn}};
    int ne = (int)(sizeof e / sizeof e[0]);
    t[p++] = (uint8_t)ne; t[p++] = 0;
    for (int i = 0; i < ne; i++) {
        t[p++] = e[i].tag & 0xff; t[p++] = e[i].tag >> 8; t[p++] = (uint8_t)e[i].ty; t[p++] = 0;
        t[p++] = 1; t[p++] = 0; t[p++] = 0; t[p++] = 0;
        t[p++] = e[i].v; t[p++] = e[i].v >> 8; t[p++] = e[i].v >> 16; t[p++] = e[i].v >> 24;
    }
    p += 4;
    *len = p; return t;
}

static int mentions(const char *m, const char *w) { return m && strstr(m, w); }

static int all_eq(const ImgResult *r, uint8_t v, uint64_t from) {
    for (uint64_t i = from; i < r->ok.data_len; i++) if (r->ok.data[i] != v) return 0;
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    const char *slow = argv[1], *seeds = argv[2];
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t n, tn;

    /* --- the slow unit, first, so the peak-RSS mark is ours --- */
    uint8_t *b = slurp(slow, &n);
    CHECK(b && n == 1686, "slow unit loaded (1,686 bytes)");
    long long r0 = rss_bytes(); double c0 = cpu_s();
    ImgResult r = image_decode(b, n);
    double dc = cpu_s() - c0; long long grew = rss_bytes() - r0;
    printf("        decode: %.3f s CPU, peak RSS growth %lld KiB, %s\n", dc, grew / 1024,
           r.is_err ? r.err_msg : "decoded");
    CHECK(r.is_err && mentions(r.err_msg, "decompression-bomb guard"),
          "slow unit (2600 x 65435 declared over a 1,590-byte strip): refused by the bomb guard");
    CHECK(dc < 0.1, "slow unit: refused in < 0.1 s CPU");
    CHECK(grew < (8ll << 20), "slow unit: peak RSS grew < 8 MiB (no page allocated)");
    if (!r.is_err) image_buf_free(&r.ok);
    free(b);

    /* --- a large declared page over a 64-byte uncompressed strip --- */
    uint8_t small[64]; memset(small, 9, sizeof small);
    uint8_t *t = mktiff(4096, 4096, 8, 1, small, sizeof small, &tn);
    r = image_tiff_decode(t, tn, 0);
    CHECK(r.is_err && mentions(r.err_msg, "decompression-bomb guard"),
          "4096 x 4096 8-bit page over a 64-byte uncompressed strip: refused");
    if (!r.is_err) image_buf_free(&r.ok);
    free(t);

    /* --- controls --- */
    const char *good[] = {"ccitt_mh.tif", "ccitt_g3_2d.tif", "ccitt_g4.tif", "tiff_two_pages.tif"};
    for (int i = 0; i < 4; i++) {
        char p[4096]; snprintf(p, sizeof p, "%s/%s", seeds, good[i]);
        b = slurp(p, &n);
        r = image_decode(b, n);
        char m[256]; snprintf(m, sizeof m, "control: seed %s decodes", good[i]);
        CHECK(!r.is_err, m);
        if (!r.is_err) image_buf_free(&r.ok);
        free(b);
    }

    {   /* full uncompressed 2600 x 2000 */
        size_t dn = 2600u * 2000u; uint8_t *d = malloc(dn);
        for (size_t i = 0; i < dn; i++) d[i] = (uint8_t)(i * 7);
        t = mktiff(2600, 2000, 8, 1, d, dn, &tn);
        r = image_tiff_decode(t, tn, 0);
        CHECK(!r.is_err && r.ok.data_len == dn && !memcmp(r.ok.data, d, dn),
              "control: full 2600 x 2000 uncompressed page decodes exactly");
        if (!r.is_err) image_buf_free(&r.ok);
        free(t); free(d);
    }
    {   /* 4096 x 4096 zeros, Deflate level 9 */
        size_t dn = 4096u * 4096u; uint8_t *d = calloc(1, dn);
        uLongf zn = compressBound(dn); uint8_t *z = malloc(zn);
        compress2(z, &zn, d, dn, 9);
        printf("        zlib-9 strip: %lu bytes for %zu (ratio %.0f:1)\n", (unsigned long)zn, dn, (double)dn / zn);
        t = mktiff(4096, 4096, 8, 8, z, zn, &tn);
        r = image_tiff_decode(t, tn, 0);
        CHECK(!r.is_err && r.ok.data_len == dn && all_eq(&r, 0, 0),
              "control: 4096 x 4096 Deflate (zlib 9, near DEFLATE's 1032:1 ceiling) decodes");
        if (!r.is_err) image_buf_free(&r.ok);
        free(t); free(z); free(d);
    }
    {   /* 4096 x 256 PackBits at its ceiling: every 2 bytes repeat 128 */
        size_t dn = 4096u * 256u, zn = dn / 64; uint8_t *z = malloc(zn);
        for (size_t i = 0; i < zn; i += 2) { z[i] = (uint8_t)(int8_t)-127; z[i + 1] = 200; }
        t = mktiff(4096, 256, 8, 32773, z, zn, &tn);
        r = image_tiff_decode(t, tn, 0);
        CHECK(!r.is_err && r.ok.data_len == dn && all_eq(&r, 200, 0),
              "control: 4096 x 256 PackBits at 128 bytes per 2 decodes");
        if (!r.is_err) image_buf_free(&r.ok);
        free(t); free(z);
    }
    {   /* 64 x 64 uncompressed with only 1 KiB of the 4 KiB strip present */
        uint8_t d[1024]; memset(d, 77, sizeof d);
        t = mktiff(64, 64, 8, 1, d, sizeof d, &tn);
        r = image_tiff_decode(t, tn, 0);
        CHECK(!r.is_err && r.ok.data_len == 4096 && r.ok.data[1023] == 77 && all_eq(&r, 0, 1024),
              "control: a short strip within the 1 MiB slack still zero-fills (unchanged)");
        if (!r.is_err) image_buf_free(&r.ok);
        free(t);
    }

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

if ! "${CC}" -std=c11 -D_GNU_SOURCE -O2 -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" \
        "${S}/image.c" -lz -lm >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
"${WORK}/t" "${SLOW}" "${SEEDS}"
rc=$?

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/ts" "${WORK}/t.c" \
        "${S}/image.c" -lz -lm >"${WORK}/ccs.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/ccs.log")"
    exit 1
fi
# Under ASan the CPU bound is not meaningful; every other check must hold
# and no sanitizer report may appear.
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/ts" "${SLOW}" "${SEEDS}" >"${WORK}/asan.log" 2>&1
if ! grep -a 'FAIL' "${WORK}/asan.log" | grep -v -q 'in < 0.1 s CPU' \
   && ! grep -a -q -E 'runtime error|ERROR: AddressSanitizer' "${WORK}/asan.log" \
   && grep -a -q '^Results:' "${WORK}/asan.log"; then
    echo "  PASS: the same checks run clean under ASan+UBSan (no sanitizer report)"
else
    echo "  FAIL: ASan+UBSan run:"; grep -a -E 'FAIL|ERROR|runtime error' "${WORK}/asan.log" | head -6 | sed 's/^/      /'
    rc=1
fi

# The fuzz harness (standalone engine, -O2) replays the slow unit in bounded CPU.
if ! "${CC}" -std=c99 -D_GNU_SOURCE -O2 -o "${WORK}/fz" "${REPO_ROOT}/test/fuzz/standalone_main.c" \
        "${REPO_ROOT}/test/fuzz/fuzz_image.c" "${S}/image.c" -lm >"${WORK}/fcc.log" 2>&1; then
    echo "  FAIL: fuzz_image standalone build: $(head -c 800 "${WORK}/fcc.log")"
    exit 1
fi
mkdir -p "${WORK}/one" && cp "${SLOW}" "${WORK}/one/"
cpu=$(python3 - "${WORK}/fz" "${WORK}/one" <<'PY'
import resource, subprocess, sys
subprocess.run([sys.argv[1], sys.argv[2]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
r = resource.getrusage(resource.RUSAGE_CHILDREN)
print("%.3f %d" % (r.ru_utime + r.ru_stime, r.ru_maxrss))
PY
)
secs=${cpu% *}; maxrss=${cpu#* }
if python3 -c "import sys; sys.exit(0 if float('${secs}') < 1.0 else 1)"; then
    echo "  PASS: fuzz_image replays the slow unit in ${secs} s CPU (< 1 s; was 3.2 s)"
else
    echo "  FAIL: fuzz_image replay of the slow unit took ${secs} s CPU (bound 1 s)"
    rc=1
fi
echo "        harness child peak RSS: ${maxrss} (ru_maxrss units)"
echo ""
[ "${rc}" -eq 0 ] && echo "C2030: PASS" || echo "C2030: FAIL (exit ${rc})"
exit "${rc}"
