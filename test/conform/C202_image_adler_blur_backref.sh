#!/usr/bin/env bash
# C202_image_adler_blur_backref.sh — std.image checks the PNG zlib Adler-32,
# bounds image.blur's pass count, and its inflater refuses a zero distance
# (stories 122.13 (4), 135.3j (5)).
#
# THE DEFECTS (measured on main before 122.13).
#   Adler-32   image_decode stripped the zlib trailer and never compared it:
#              a PNG whose chunk CRCs are all correct but whose inflated data
#              does not match its Adler-32 decoded, silently, to whatever the
#              corrupted stream inflated to.
#   blur       image_blur(img, radius) ran `radius` full 3x3 passes with no
#              upper bound (image.blur(img; 4000000000) pins a CPU for hours);
#              tk_image_blur_w cast the u32 pass count to int, so 3e9 wrapped
#              negative and ran ONE pass instead of being refused.
#   backref    outbuf_push_backref(ob, 0, n) copied ob->buf[ob->len], the byte
#              being written: a heap read past the end when len == cap.  No
#              caller passes 0 today (dist_base starts at 1), so the driver
#              reaches the static function by including image.c.
#
# WHAT THIS DRIVES (ASan+UBSan build):
#   a valid PNG decodes (control); the same PNG with the Adler-32 flipped and
#   the IDAT CRC recomputed is refused with an Adler-32 error; blur at the
#   limit (256) returns a buffer and 257 returns an empty one; the toke
#   wrapper with passes 3e9 returns an empty image; backref(dist 0) sets the
#   error and reads nothing.  The pass-count checks never run a long blur,
#   so main fails them on the RESULT (a non-empty buffer), not by hanging.
#
# Negative control: C202_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's image.c, image.h and image_glue.c fails the Adler-32,
# both blur-limit and the backref checks.
#
# Story: 122.13, 135.3j

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C202_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c202_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C202: image Adler-32 gate, blur pass bound, zero-distance backref"
echo "-----------------------------------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "image.c"          /* for the static outbuf_push_backref */
#include "bytes_rt.h"

_Thread_local int64_t tk_current_error;
int64_t tk_image_fromraw_w(int64_t bytes, int64_t width, int64_t height, int64_t channels);
int64_t tk_image_blur_w(int64_t buf, int64_t passes);
int64_t tk_image_pixelat_w(int64_t buf, int64_t x, int64_t y);

#ifndef IMG_BLUR_MAX_PASSES
#define IMG_BLUR_MAX_PASSES 256    /* main has no limit; test its behaviour at ours */
#endif

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

static void put32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static size_t chunk(uint8_t *o, const char *t, const uint8_t *d, uint32_t n) {
    put32(o, n); memcpy(o + 4, t, 4); if (n) memcpy(o + 8, d, n);
    put32(o + 8 + n, (uint32_t)crc32(crc32(0, (const Bytef *)t, 4), d ? d : (const Bytef *)"", n));
    return 12 + n;
}
/* a 4x2 RGB PNG from zlib-compressed rows; flip != 0 corrupts the Adler-32 */
static uint8_t *mkpng(int flip, size_t *len) {
    uint8_t raw[2 * (1 + 4 * 3)];
    for (size_t i = 0; i < sizeof raw; i++) raw[i] = (uint8_t)(i * 7);
    raw[0] = raw[13] = 0;                               /* filter None per row */
    uLongf zn = compressBound(sizeof raw); uint8_t z[256];
    compress2(z, &zn, raw, sizeof raw, 9);
    if (flip) z[zn - 1] ^= 0x5a;                        /* the Adler-32's low byte */
    uint8_t *o = malloc(zn + 64), ih[13]; size_t p = 8;
    memcpy(o, "\x89PNG\r\n\x1a\n", 8);
    put32(ih, 4); put32(ih + 4, 2); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    p += chunk(o + p, "IHDR", ih, 13);
    p += chunk(o + p, "IDAT", z, (uint32_t)zn);          /* CRC over the corrupted bytes: valid */
    p += chunk(o + p, "IEND", NULL, 0);
    *len = p; return o;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    size_t n;
    uint8_t *good = mkpng(0, &n);
    ImgResult r = image_decode(good, n);
    CHECK(!r.is_err && r.ok.width == 4 && r.ok.height == 2, "valid PNG decodes (control)");
    if (!r.is_err) image_buf_free(&r.ok);
    free(good);

    uint8_t *bad = mkpng(1, &n);
    r = image_decode(bad, n);
    CHECK(r.is_err && r.err_msg && strstr(r.err_msg, "Adler-32"),
          "PNG with correct CRCs but a wrong zlib Adler-32 is refused, naming Adler-32");
    if (!r.is_err) image_buf_free(&r.ok);
    free(bad);

    TkImgBuf img; memset(&img, 0, sizeof img);
    uint8_t px[2 * 2 * 3] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120 };
    img.width = 2; img.height = 2; img.channels = 3; img.data = px; img.data_len = sizeof px;
    TkImgBuf b = image_blur(img, IMG_BLUR_MAX_PASSES);
    CHECK(b.data != NULL && b.width == 2, "image_blur at the limit returns an image");
    image_buf_free(&b);
    b = image_blur(img, IMG_BLUR_MAX_PASSES + 1);
    CHECK(b.data == NULL && b.width == 0, "image_blur past the limit is refused (empty buffer)");
    image_buf_free(&b);

    int64_t h = tk_image_fromraw_w(tk_bytes_pack(px, sizeof px), 2, 2, 3);
    int64_t bl = h ? tk_image_blur_w(h, 3000000000LL) : 0;
    /* an empty $imgbuf has width 0: its first field */
    CHECK(bl != 0 && ((int64_t *)(intptr_t)bl)[0] == 0,
          "image.blur(img; 3000000000) through the toke wrapper is refused, not wrapped to 1 pass");

    OutBuf ob; outbuf_init(&ob);
    outbuf_push(&ob, 'a');
    while (ob.len < ob.cap) outbuf_push(&ob, 'b');      /* len == cap: buf[len] is past the end */
    outbuf_push_backref(&ob, 0, 1);
    CHECK(ob.error == 1, "outbuf_push_backref(dist 0) is an error and reads nothing");
    free(ob.buf);

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" \
        "${S}/image_glue.c" -lz -lm >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t"
rc=$?
echo ""
[ "${rc}" -eq 0 ] && echo "C202: PASS" || echo "C202: FAIL (exit ${rc})"
exit "${rc}"
