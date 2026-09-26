#!/usr/bin/env bash
# C138_image_decoder_hardening.sh — std.image decodes untrusted bytes safely
# (story 122.9).
#
# THE DEFECTS (all measured on main before 122.9):
#   1. PNG chunk CRCs were never checked ("we skip CRC validation").
#   2. The inflater had no output cap: a 1x1 PNG whose IDAT inflates to
#      64 MiB was inflated in full and only then rejected on size, and a
#      TIFF Deflate strip was inflated in full and then cut to the strip.
#      A decompression bomb costs the attacker ~65 KB.
#   3. IHDR dimensions were unbounded (65536 x 65536 RGBA accepted as a
#      16 GiB target before a byte was inflated).
#   4. No transform checked a TkImgBuf against its own buffer.  $imgbuf is a
#      plain toke struct, so `width*height*channels > len(data)` was a heap
#      over-read in resize/flip/rotate/blur/crop/encode/pixelat/...; and
#      image.fromraw dropped the array's length (short array -> over-read).
#   5. rotate cast a NaN coordinate to int (UB); textdraw overflowed int
#      for a large size.
#
# WHAT THIS DRIVES (built -fsanitize=address,undefined, no recover):
#   - the tracked seeds decode to the exact pixels (all five PNG filters,
#     fixed/dynamic/stored DEFLATE, TIFF none + Deflate, page 2)
#   - bad CRC -> error naming the CRC
#   - PNG bomb -> error naming the bomb, peak RSS grows < 24 MiB
#   - TIFF Deflate bomb -> decodes (strip cut at its size), peak RSS < 24 MiB
#   - 65536x65536 IHDR -> error naming the cap
#   - a TkImgBuf claiming 100x100x4 over 16 bytes -> every transform refuses
#   - tk_image_fromraw_w over a 16-byte array for 100x100x4 -> zero-padded
#   - rotate(NaN), textdraw(size 2^30) -> no UB, bounded time
#   - test/fuzz/fuzz_image.c, linked standalone, replays the seeds and the
#     two generated bombs clean.
# Leaks are not checked (macOS has no LeakSanitizer).
#
# Negative control (122.9): restore `// skip CRC validation` -> "bad CRC";
# drop the outbuf limit check -> "PNG bomb" RSS + message; drop img_buf_ok
# from image_flip_h -> ASan heap-buffer-overflow.
#
# Story: 122.9

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
SEEDS="${REPO_ROOT}/test/fuzz/seeds/image"
HARNESS="${REPO_ROOT}/test/fuzz/fuzz_image.c"
STANDALONE="${REPO_ROOT}/test/fuzz/standalone_main.c"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c138_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C138: std.image decoder hardening (122.9)"
echo "-----------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/resource.h>
#include <zlib.h>
#include "image.h"
#include "bytes_rt.h"

_Thread_local int64_t tk_current_error;
int64_t tk_image_fromraw_w(int64_t bytes, int64_t width, int64_t height, int64_t channels);
int64_t tk_image_pixelat_w(int64_t buf, int64_t x, int64_t y);

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

static uint8_t *slurp(const char *dir, const char *name, size_t *n) {
    char p[4096]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { *n = 0; return NULL; }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1); *n = fread(b, 1, (size_t)l, f); fclose(f);
    return b;
}

/* zlib stream of n zero bytes, built without ever holding n bytes */
static uint8_t *zbomb(uint64_t n, size_t *outlen) {
    static uint8_t zero[65536];
    z_stream z; memset(&z, 0, sizeof z);
    deflateInit(&z, 9);
    size_t cap = (size_t)(n / 500) + 4096;
    uint8_t *o = malloc(cap);
    z.next_out = o; z.avail_out = (uInt)cap;
    uint64_t left = n; int rc;
    do {
        size_t k = left > sizeof zero ? sizeof zero : (size_t)left;
        z.next_in = zero; z.avail_in = (uInt)k; left -= k;
        rc = deflate(&z, left ? Z_NO_FLUSH : Z_FINISH);
    } while (left);
    while (rc != Z_STREAM_END) rc = deflate(&z, Z_FINISH);
    *outlen = cap - z.avail_out;
    deflateEnd(&z);
    return o;
}

static void put32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static size_t chunk(uint8_t *o, const char *t, const uint8_t *d, uint32_t n) {
    put32(o, n); memcpy(o + 4, t, 4); if (n) memcpy(o + 8, d, n);
    put32(o + 8 + n, (uint32_t)crc32(crc32(0, (const Bytef *)t, 4), d ? d : (const Bytef *)"", n));
    return 12 + n;
}
static uint8_t *mkpng(uint32_t w, uint32_t h, uint8_t ct, const uint8_t *z, size_t zn, size_t *len) {
    uint8_t *o = malloc(zn + 64), ih[13]; size_t p = 0;
    memcpy(o, "\x89PNG\r\n\x1a\n", 8); p = 8;
    put32(ih, w); put32(ih + 4, h); ih[8] = 8; ih[9] = ct; ih[10] = ih[11] = ih[12] = 0;
    p += chunk(o + p, "IHDR", ih, 13);
    p += chunk(o + p, "IDAT", z, (uint32_t)zn);
    p += chunk(o + p, "IEND", NULL, 0);
    *len = p; return o;
}

static int mentions(const char *msg, const char *word) { return msg && strstr(msg, word); }

int main(int argc, char **argv) {
    const char *seeds = argv[1], *work = argv[2];
    (void)argc;
    size_t n, zn, pn;
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep the trail when ASan aborts */

    /* --- bombs first, so the peak-RSS mark is ours --- */
    uint8_t *z = zbomb(64u << 20, &zn);
    uint8_t *bomb = mkpng(1, 1, 2, z, zn, &pn);
    { char p[4096]; snprintf(p, sizeof p, "%s/png_bomb_64mib.png", work);
      FILE *f = fopen(p, "wb"); fwrite(bomb, 1, pn, f); fclose(f); }
    long long r0 = rss_bytes();
    ImgResult r = image_decode(bomb, pn);
    long long grew = rss_bytes() - r0;
    CHECK(r.is_err && mentions(r.err_msg, "bomb"), "PNG bomb (1x1, IDAT inflates to 64 MiB): refused as a bomb");
    printf("        peak RSS growth: %lld KiB\n", grew / 1024);
    CHECK(grew < (24ll << 20), "PNG bomb: peak RSS grew < 24 MiB (the inflate stopped at 4 bytes)");
    free(bomb);

    /* TIFF, one 4x4 8-bit strip, Deflate, whose stream inflates to 64 MiB */
    {
        uint8_t *t = calloc(1, zn + 256); size_t p = 8;
        memcpy(t, "II*\0", 4);
        memcpy(t + p, z, zn); size_t so = p; p += zn; if (p & 1) p++;
        uint32_t ifd = (uint32_t)p; t[4]=ifd; t[5]=ifd>>8; t[6]=ifd>>16; t[7]=ifd>>24;
        uint16_t tags[9][2] = {{256,4},{257,4},{258,8},{259,8},{262,1},{273,0},{277,1},{278,4},{279,0}};
        t[p++] = 9; t[p++] = 0;
        for (int i = 0; i < 9; i++) {
            uint32_t v = tags[i][1]; uint16_t ty = 3;
            if (tags[i][0] == 273) { v = (uint32_t)so; ty = 4; }
            if (tags[i][0] == 279) { v = (uint32_t)zn; ty = 4; }
            t[p++] = tags[i][0] & 0xff; t[p++] = tags[i][0] >> 8; t[p++] = ty; t[p++] = 0;
            t[p++] = 1; t[p++] = 0; t[p++] = 0; t[p++] = 0;
            t[p++] = v; t[p++] = v >> 8; t[p++] = v >> 16; t[p++] = v >> 24;
        }
        p += 4;
        char fp[4096]; snprintf(fp, sizeof fp, "%s/tiff_deflate_bomb.tif", work);
        FILE *f = fopen(fp, "wb"); fwrite(t, 1, p, f); fclose(f);
        r0 = rss_bytes();
        ImgResult tr = image_decode(t, p);
        grew = rss_bytes() - r0;
        CHECK(!tr.is_err && tr.ok.width == 4 && tr.ok.data_len == 16,
              "TIFF Deflate bomb strip: decodes, cut at the strip's 16 bytes");
        printf("        peak RSS growth: %lld KiB\n", grew / 1024);
        CHECK(grew < (24ll << 20), "TIFF Deflate bomb: peak RSS grew < 24 MiB");
        if (!tr.is_err) image_buf_free(&tr.ok);
        free(t);
    }
    free(z);

    /* --- tracked seeds decode to the exact pixels --- */
    uint8_t *b = slurp(seeds, "png_rgb_all_filters.png", &n);
    r = image_decode(b, n);
    int exact = !r.is_err && r.ok.width == 24 && r.ok.height == 24 && r.ok.channels == 3;
    for (int y = 0; exact && y < 24; y++)
        for (int x = 0; x < 24; x++)
            for (int c = 0; c < 3; c++)
                if (r.ok.data[(y * 24 + x) * 3 + c] != (uint8_t)((x * 37 + y * 11 + c * 5) & 0xff)) exact = 0;
    CHECK(exact, "seed png_rgb_all_filters (24x24, rows filtered 0-4, dynamic-Huffman IDAT): decodes exactly");
    if (!r.is_err) image_buf_free(&r.ok);
    free(b);

    const char *good[] = {"png_gray_alpha_fixed.png", "png_rgba_stored.png", "png_gray.png", "tiff_two_pages.tif"};
    for (int i = 0; i < 4; i++) {
        b = slurp(seeds, good[i], &n);
        r = image_decode(b, n);
        char m[256]; snprintf(m, sizeof m, "seed %s decodes", good[i]);
        CHECK(!r.is_err, m);
        if (!r.is_err) image_buf_free(&r.ok);
        if (i == 3) {
            ImgResult p2 = image_tiff_decode(b, n, 1);
            CHECK(!p2.is_err && p2.ok.data_len == 16 && p2.ok.data[1] == 13,
                  "seed tiff_two_pages: page 2 (Deflate) decodes exactly");
            if (!p2.is_err) image_buf_free(&p2.ok);
        }
        free(b);
    }

    b = slurp(seeds, "png_bad_crc.png", &n);
    r = image_decode(b, n);
    CHECK(r.is_err && mentions(r.err_msg, "CRC"), "bad IDAT CRC: refused, message names the CRC");
    free(b);

    b = slurp(seeds, "png_huge_dims.png", &n);
    r = image_decode(b, n);
    CHECK(r.is_err && mentions(r.err_msg, "cap"), "IHDR 65536x65536 RGBA: refused by the size cap before inflating");
    free(b);

    b = slurp(seeds, "png_zero_width.png", &n);
    r = image_decode(b, n);
    CHECK(r.is_err, "IHDR width 0: refused");
    free(b);

    b = slurp(seeds, "png_truncated.png", &n);
    r = image_decode(b, n);
    CHECK(r.is_err, "truncated PNG: refused");
    free(b);

    /* --- a $imgbuf whose dimensions exceed its buffer --- */
    TkImgBuf lie; memset(&lie, 0, sizeof lie);
    lie.width = 100; lie.height = 100; lie.channels = 4;
    lie.data = malloc(16); memset(lie.data, 7, 16); lie.data_len = 16;
    int refused = 1;
    TkImgBuf o;
    o = image_resize(lie, 50, 50);       refused &= o.data == NULL;
    o = image_flip_h(lie);               refused &= o.data == NULL;
    o = image_flip_v(lie);               refused &= o.data == NULL;
    o = image_rotate(lie, 30.0);         refused &= o.data == NULL;
    o = image_blur(lie, 1);              refused &= o.data == NULL;
    o = image_sharpen(lie);              refused &= o.data == NULL;
    o = image_brightness(lie, 1.5);      refused &= o.data == NULL;
    o = image_contrast(lie, 1.5);        refused &= o.data == NULL;
    o = image_to_grayscale(lie);         refused &= o.data == NULL;
    o = image_quantize(lie, 4);          refused &= o.data == NULL;
    o = image_text_draw(lie, "hi", 0, 0, 1, 0xffffffffu); refused &= o.data == NULL;
    o = image_paste(lie, lie, 0, 0);     refused &= o.data == NULL;
    ImgResult cr = image_crop(lie, 90, 90, 10, 10);           refused &= cr.is_err;
    PixelResult pr = image_pixel_at(lie, 99, 99);             refused &= pr.is_err;
    ImgEncResult e1 = image_encode(lie, IMG_FMT_PNG, 0);      refused &= e1.is_err;
    ImgEncResult e2 = image_encode(lie, IMG_FMT_BMP, 0);      refused &= e2.is_err;
    ImgHistogram hg = image_histogram(lie);                   refused &= hg.r[7] == 0;
    CHECK(refused, "TkImgBuf 100x100x4 over 16 bytes: all 17 transforms refuse it (no over-read)");
    free(lie.data);

    int64_t h = tk_image_fromraw_w(tk_bytes_pack((const uint8_t *)"0123456789abcdef", 16), 100, 100, 4);
    int64_t px = h ? tk_image_pixelat_w(h, 99, 99) : 0;
    uint8_t *pb = NULL; uint64_t pl = px ? tk_bytes_unpack(px, &pb) : 0;
    CHECK(pl == 4 && pb && pb[0] == 0 && pb[3] == 0,
          "image.fromraw over a 16-byte array for 100x100x4: zero-padded, no over-read");
    free(pb);
    CHECK(tk_image_fromraw_w(tk_bytes_pack((const uint8_t *)"x", 1), -1, 1, 1) != 0,
          "image.fromraw with width -1: an empty $imgbuf, not a 4-billion-wide one");

    /* --- no UB on hostile scalars --- */
    uint8_t px16[4 * 4 * 3]; memset(px16, 90, sizeof px16);
    TkImgBuf ok = image_from_raw(px16, 4, 4, 3);
    o = image_rotate(ok, NAN);
    CHECK(o.data != NULL, "rotate by NaN: no UB (all pixels out of range, image returned)");
    image_buf_free(&o);
    o = image_brightness(ok, NAN);
    CHECK(o.data != NULL && o.data[0] == 0, "brightness by NaN: clamps to 0, no UB");
    image_buf_free(&o);
    o = image_text_draw(ok, "WWWW", 0, 0, 1 << 30, 0xff0000ffu);
    CHECK(o.data != NULL && o.data[0] == 0xff, "textdraw with size 2^30: no int overflow, clipped, bounded time");
    image_buf_free(&o);
    image_buf_free(&ok);

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" \
        "${S}/image.c" "${S}/image_glue.c" -lz -lm >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
echo "  PASS: built with ${SAN}"
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" "${SEEDS}" "${WORK}"
rc=$?
[ -f "${WORK}/png_bomb_64mib.png" ] || { echo "  FAIL: the driver did not write the generated bombs"; exit 1; }

# The fuzz harness, standalone engine: a replay of the seeds + the bombs.
if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -o "${WORK}/fuzz-image" \
        "${STANDALONE}" "${HARNESS}" "${S}/image.c" -lm >"${WORK}/fcc.log" 2>&1; then
    echo "  FAIL: fuzz_image standalone build: $(head -c 800 "${WORK}/fcc.log")"
    exit 1
fi
mkdir -p "${WORK}/gen"
mv "${WORK}/png_bomb_64mib.png" "${WORK}/tiff_deflate_bomb.tif" "${WORK}/gen/"
nseeds=$(ls "${SEEDS}" | wc -l | tr -d ' ')
if out=$(ASAN_OPTIONS="detect_leaks=0" "${WORK}/fuzz-image" "${SEEDS}" "${WORK}/gen" 2>&1); then
    echo "  PASS: fuzz_image (standalone, a replay -- not fuzzing) ran ${nseeds} seeds + 2 generated bombs clean: $(printf '%s' "$out" | tail -1)"
else
    echo "  FAIL: fuzz_image replay:"
    printf '%s\n' "$out" | /usr/bin/grep -a -E 'ERROR: |runtime error|SUMMARY|fuzz-replay' | head -6 | sed 's/^/      /'
    rc=1
fi
exit $rc
