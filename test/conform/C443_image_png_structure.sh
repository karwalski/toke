#!/usr/bin/env bash
# C443_image_png_structure.sh — std.image's PNG decoder refuses what the PNG
# and zlib/DEFLATE formats forbid, instead of decoding it to wrong pixels
# (story 121.33, the residue after 122.9 / 122.13).
#
# THE DEFECTS (measured on main before 121.33).  Each input below decoded
# on main -- no error, and in the first two the pixels were wrong:
#   interlace   IHDR's interlace byte was never read.  An Adam7 image whose
#               passes add up to the non-interlaced size (1x3 is one)
#               decoded with its rows in pass order: 10,30,20 for 10,20,30.
#   two IHDRs   a second IHDR after the IDAT replaced the geometry the IDAT
#               was decoded against (a 2x1 grey image came back as 1x1
#               grey+alpha).
#   methods     IHDR compression/filter method bytes were never read.
#   zlib hdr    the two zlib header bytes were skipped unread: a bad FCHECK,
#               a method other than 8, or a preset dictionary all inflated.
#   Huffman     the canonical-Huffman builder accepted an over-subscribed
#               code-length code (three 1-bit codes) that zlib rejects as
#               "invalid code lengths set".
# Memory safety was already covered (C138, C202); these are wrong-output
# defects in the same decoder, and every refusal is a static error string.
#
# WHAT THIS DRIVES (ASan+UBSan build of image.c, inputs built with zlib):
#   each defective input is refused with an error; controls: a 1x1 grey PNG,
#   a 64x64 RGBA PNG compressed by zlib at levels 1..9 (dynamic Huffman
#   blocks) and the same image with filter types 0..4 decode to the exact
#   pixels; then every test/fuzz/corpus-adversarial/image_* file (written by
#   this driver, see C443_GEN) is refused.
#
# Regenerating the corpus files: C443_GEN=<dir> writes them to <dir>.
# Negative control: C443_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's image.c fails the first seven refusal checks (the last
# three are regressions main already passes).
#
# Story: 121.33

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C443_STDLIB:-${REPO_ROOT}/src/stdlib}"
ADV="${REPO_ROOT}/test/fuzz/corpus-adversarial"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c443_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C443: PNG decoder refuses interlace, repeated IHDR, bad zlib header, bad Huffman"
echo "-------------------------------------------------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "image.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* ---- PNG assembly ---- */
typedef struct { uint8_t b[1 << 16]; size_t n; } Png;
static void put32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void png_start(Png *p) { static const uint8_t sig[8] = {137,80,78,71,13,10,26,10};
    memcpy(p->b, sig, 8); p->n = 8; }
static void png_chunk(Png *p, const char *t, const uint8_t *d, uint32_t n) {
    put32(p->b + p->n, n); memcpy(p->b + p->n + 4, t, 4);
    if (n) memcpy(p->b + p->n + 8, d, n);
    put32(p->b + p->n + 8 + n,
          (uint32_t)crc32(crc32(0, (const Bytef *)t, 4), d ? d : (const Bytef *)"", n));
    p->n += 12 + n;
}
static void ihdr(Png *p, uint32_t w, uint32_t h, uint8_t ct, uint8_t cm, uint8_t fm, uint8_t il) {
    uint8_t d[13]; put32(d, w); put32(d + 4, h);
    d[8] = 8; d[9] = ct; d[10] = cm; d[11] = fm; d[12] = il;
    png_chunk(p, "IHDR", d, 13);
}
static void idat_z(Png *p, const uint8_t *raw, size_t n, int level) {
    uLongf zn = compressBound(n); uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, n, level);
    png_chunk(p, "IDAT", z, (uint32_t)zn); free(z);
}
static void iend(Png *p) { png_chunk(p, "IEND", NULL, 0); }

/* ---- raw DEFLATE bit writer (LSB-first fields; Huffman codes MSB-first) ---- */
typedef struct { uint8_t b[4096]; size_t bit; } Bits;
static void bput(Bits *w, uint32_t v, int n) {
    for (int i = 0; i < n; i++, w->bit++)
        if ((v >> i) & 1) w->b[w->bit >> 3] |= (uint8_t)(1u << (w->bit & 7));
}
static void bcode(Bits *w, uint32_t code, int n) {       /* MSB of code first */
    for (int i = n - 1; i >= 0; i--) bput(w, (code >> i) & 1, 1);
}
/* zlib-wrap a raw deflate stream whose inflation is `plain` */
static size_t zwrap(uint8_t *out, uint8_t cmf, uint8_t flg, const Bits *w,
                    const uint8_t *plain, size_t pn) {
    size_t n = (w->bit + 7) / 8;
    out[0] = cmf; out[1] = flg; memcpy(out + 2, w->b, n);
    put32(out + 2 + n, (uint32_t)adler32(adler32(0, NULL, 0), plain, (uInt)pn));
    return n + 6;
}

/* ---- the cases ---- */
enum { C_INTERLACE, C_TWO_IHDR, C_METHOD, C_FCHECK, C_CM7, C_FDICT, C_OVERSUB,
       C_BOMB, C_STRIDE, C_BACKREF, NCASES };
static const char *NAMES[NCASES] = {
    "image_interlaced_1x3.png", "image_second_ihdr.png", "image_filter_method_1.png",
    "image_zlib_bad_fcheck.png", "image_zlib_method_7.png", "image_zlib_fdict.png",
    "image_huffman_oversubscribed.png", "image_bomb_1mib.png",
    "image_stride_overflow.png", "image_backref_before_start.png" };

static void build(int c, Png *p) {
    memset(p, 0, sizeof *p);
    png_start(p);
    switch (c) {
    case C_INTERLACE: {         /* 1x3 grey 10,20,30 as Adam7: pass1 y0, pass5 y2, pass7 y1 */
        ihdr(p, 1, 3, 0, 0, 0, 1);
        uint8_t raw[] = { 0, 10, 0, 30, 0, 20 };
        idat_z(p, raw, sizeof raw, 9); break; }
    case C_TWO_IHDR: {          /* 2x1 grey, then IHDR says 1x1 grey+alpha */
        ihdr(p, 2, 1, 0, 0, 0, 0);
        uint8_t raw[] = { 0, 1, 2 };
        idat_z(p, raw, sizeof raw, 9);
        ihdr(p, 1, 1, 4, 0, 0, 0); break; }
    case C_METHOD: {
        ihdr(p, 1, 1, 0, 0, 1, 0);
        uint8_t raw[] = { 0, 7 };
        idat_z(p, raw, sizeof raw, 9); break; }
    case C_FCHECK: case C_CM7: case C_FDICT: {
        ihdr(p, 1, 1, 0, 0, 0, 0);
        uint8_t raw[] = { 0, 7 };
        uLongf zn = 64; uint8_t z[64];
        compress2(z, &zn, raw, sizeof raw, 9);
        if (c == C_FCHECK) z[1] ^= 0x01;                   /* (CMF*256+FLG) % 31 != 0 */
        if (c == C_CM7)   { z[0] = 0x77; z[1] = 0; while (((0x77u << 8) | z[1]) % 31) z[1]++; }
        if (c == C_FDICT) { z[0] = 0x78; z[1] = 0x20; while (((0x78u << 8) | z[1]) % 31) z[1]++; }
        png_chunk(p, "IDAT", z, (uint32_t)zn); break; }
    case C_OVERSUB: {           /* 1x1 grey 0; code-length code has three 1-bit codes */
        ihdr(p, 1, 1, 0, 0, 0, 0);
        Bits w; memset(&w, 0, sizeof w);
        bput(&w, 1, 1); bput(&w, 2, 2);                    /* BFINAL, BTYPE=2 */
        bput(&w, 0, 5); bput(&w, 0, 5); bput(&w, 14, 4);   /* HLIT 257, HDIST 1, HCLEN 18 */
        /* order 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1: 18, 0 and 1 get length 1 */
        static const uint8_t cl[18] = { 0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,1 };
        for (int i = 0; i < 18; i++) bput(&w, cl[i], 3);
        /* 258 lengths: lit 0 -> 1, 1..255 -> 0, 256 -> 1, dist 0 -> 0
         * (code-length symbol 0 is code '0', symbol 1 is code '1') */
        bcode(&w, 1, 1);
        for (int i = 1; i < 256; i++) bcode(&w, 0, 1);
        bcode(&w, 1, 1); bcode(&w, 0, 1);
        bcode(&w, 0, 1); bcode(&w, 0, 1); bcode(&w, 1, 1); /* lit 0, lit 0, end of block */
        uint8_t z[128]; uint8_t plain[2] = { 0, 0 };
        size_t zn = zwrap(z, 0x78, 0x01, &w, plain, 2);
        png_chunk(p, "IDAT", z, (uint32_t)zn); break; }
    case C_BOMB: {              /* 16x16 grey, 1 MiB of zeros in the IDAT */
        ihdr(p, 16, 16, 0, 0, 0, 0);
        size_t n = 1u << 20; uint8_t *raw = calloc(n, 1);
        idat_z(p, raw, n, 9); free(raw); break; }
    case C_STRIDE: {            /* width*1 = 2^30+1 bytes: past the 1 GiB decoded cap */
        ihdr(p, 0x40000001u, 1, 0, 0, 0, 0);
        uint8_t raw[] = { 0, 0 };
        idat_z(p, raw, sizeof raw, 9); break; }
    case C_BACKREF: {           /* fixed Huffman: a length-3/distance-1 copy with no output yet */
        ihdr(p, 1, 1, 0, 0, 0, 0);
        Bits w; memset(&w, 0, sizeof w);
        bput(&w, 1, 1); bput(&w, 1, 2);                    /* BFINAL, BTYPE=1 */
        bcode(&w, 0x01, 7);                                /* 257: length 3 */
        bcode(&w, 0, 5);                                   /* distance code 0: 1 */
        bcode(&w, 0x00, 7);                                /* 256: end of block */
        uint8_t z[64]; uint8_t plain[3] = { 0, 0, 0 };
        size_t zn = zwrap(z, 0x78, 0x01, &w, plain, 3);
        png_chunk(p, "IDAT", z, (uint32_t)zn); break; }
    }
    iend(p);
}

static int refused(const Png *p, const char **why) {
    ImgResult r = image_decode(p->b, p->n);
    *why = r.is_err ? r.err_msg : "decoded";
    if (!r.is_err) image_buf_free(&r.ok);
    return r.is_err;
}

int main(int argc, char **argv) {
    static Png p;
    if (argc > 1 && !strcmp(argv[1], "refuse")) {      /* files that must not decode */
        int accepted = 0;
        for (int i = 2; i < argc; i++) {
            FILE *f = fopen(argv[i], "rb"); if (!f) { accepted++; continue; }
            p.n = fread(p.b, 1, sizeof p.b, f); fclose(f);
            const char *why;
            if (!refused(&p, &why)) { printf("        decoded: %s\n", argv[i]); accepted++; }
        }
        return accepted ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[1], "gen")) {
        for (int c = 0; c < NCASES; c++) {
            build(c, &p);
            char path[1024]; snprintf(path, sizeof path, "%s/%s", argv[2], NAMES[c]);
            FILE *f = fopen(path, "wb"); fwrite(p.b, 1, p.n, f); fclose(f);
        }
        return 0;
    }

    const char *why; char msg[256];
    static const char *LABEL[NCASES] = {
        "Adam7 1x3 grey (passes add up to the flat size)",
        "a second IHDR after the IDAT",
        "IHDR filter method 1",
        "zlib header with a bad FCHECK",
        "zlib header with compression method 7",
        "zlib header with a preset dictionary (FDICT)",
        "over-subscribed Huffman code-length code",
        "regression (122.9): 1 MiB IDAT for a 16x16 grey IHDR (bomb)",
        "regression (122.9): width 2^30+1 (past the decoded-size cap)",
        "regression (135.3j): a back-reference before any output" };
    for (int c = 0; c < NCASES; c++) {
        build(c, &p);
        int ok = refused(&p, &why);
        snprintf(msg, sizeof msg, "%s is refused (%s)", LABEL[c], why);
        CHECK(ok, msg);
    }
    /* what main returned for the interlaced image, for the record */
    build(C_INTERLACE, &p);
    {
        ImgResult r = image_decode(p.b, p.n);
        if (!r.is_err) {
            printf("        interlaced 1x3 decoded to rows %u,%u,%u (true image 10,20,30)\n",
                   r.ok.data[0], r.ok.data[1], r.ok.data[2]);
            image_buf_free(&r.ok);
        }
    }

    /* controls */
    Png q; memset(&q, 0, sizeof q); png_start(&q); ihdr(&q, 1, 1, 0, 0, 0, 0);
    { uint8_t raw[] = { 0, 7 }; idat_z(&q, raw, 2, 9); } iend(&q);
    ImgResult r = image_decode(q.b, q.n);
    CHECK(!r.is_err && r.ok.width == 1 && r.ok.data[0] == 7, "control: a 1x1 grey PNG decodes");
    if (!r.is_err) image_buf_free(&r.ok);

    enum { W = 64, H = 64, CH = 4 };
    static uint8_t px[W * H * CH], raw[H * (1 + W * CH)];
    for (int i = 0; i < W * H * CH; i++) px[i] = (uint8_t)((i * 131u) ^ (i >> 7) ^ ((i % 17) * 29u));
    int bad = 0;
    for (int ft = 0; ft <= 4; ft++) {
        /* filter every row with type ft (encode side of RFC 2083 §6) */
        for (int y = 0; y < H; y++) {
            uint8_t *o = raw + y * (1 + W * CH); o[0] = (uint8_t)ft;
            for (int x = 0; x < W * CH; x++) {
                int a = x >= CH ? px[y * W * CH + x - CH] : 0;
                int b = y ? px[(y - 1) * W * CH + x] : 0;
                int cc = (x >= CH && y) ? px[(y - 1) * W * CH + x - CH] : 0;
                int pr = 0;
                if (ft == 1) pr = a; else if (ft == 2) pr = b; else if (ft == 3) pr = (a + b) / 2;
                else if (ft == 4) { int pp = a + b - cc, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - cc);
                                    pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : cc); }
                o[1 + x] = (uint8_t)(px[y * W * CH + x] - pr);
            }
        }
        for (int lvl = 1; lvl <= 9; lvl++) {
            memset(&q, 0, sizeof q); png_start(&q); ihdr(&q, W, H, 6, 0, 0, 0);
            idat_z(&q, raw, sizeof raw, lvl); iend(&q);
            r = image_decode(q.b, q.n);
            if (r.is_err || r.ok.width != W || memcmp(r.ok.data, px, sizeof px) != 0) {
                printf("        filter %d level %d: %s\n", ft, lvl, r.is_err ? r.err_msg : "pixels differ");
                bad++;
            }
            if (!r.is_err) image_buf_free(&r.ok);
        }
    }
    CHECK(bad == 0, "control: 64x64 RGBA, filters 0..4 x zlib levels 1..9, decodes to the exact pixels");

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" \
        "${WORK}/t.c" "${S}/image.c" -lz -lm >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

if [ -n "${C443_GEN:-}" ]; then
    mkdir -p "${C443_GEN}" && "${WORK}/t" gen "${C443_GEN}" && echo "C443: wrote corpus files to ${C443_GEN}"
    exit $?
fi

"${WORK}/t"
rc=$?

# The committed adversarial corpus (written by C443_GEN; compress2 output
# varies across zlib builds, so the files are checked by behaviour, not
# bytes): every image_* file there is refused.
set -- "${ADV}"/image_*
if [ ! -e "$1" ]; then
    echo "  FAIL: no test/fuzz/corpus-adversarial/image_* files"; rc=1
elif "${WORK}/t" refuse "$@"; then
    echo "  PASS: all $# test/fuzz/corpus-adversarial/image_* files are refused"
else
    echo "  FAIL: a test/fuzz/corpus-adversarial/image_* file decoded"; rc=1
fi
echo ""
[ "${rc}" -eq 0 ] && echo "C443: PASS" || echo "C443: FAIL (exit ${rc})"
exit "${rc}"
