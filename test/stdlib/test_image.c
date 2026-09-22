/*
 * test_image.c — Unit tests for the std.image C library (Story 18.1.6).
 *
 * Build and run: make test-stdlib-image
 *
 * Coverage:
 *   1.  image_from_raw: creates a 4x4 RGBA buffer
 *   2.  image_pixel_at happy path: pixel (0,0) returns 4 bytes
 *   3.  image_pixel_at out-of-bounds: x >= width → is_err=1
 *   4.  image_to_grayscale: 4x4 RGBA → 4x4 grey (channels=1)
 *   5.  image_flip_h: 1-row, 2-pixel image → pixels swapped
 *   6.  image_flip_v: 2-row, 1-pixel image → rows swapped
 *   7.  image_resize: 4x4 → 2x2, verify new dimensions
 *   8.  image_crop valid: 4x4 → crop(1,1,2,2) → 2x2
 *   9.  image_crop out-of-bounds: x+w > width → is_err=1
 *  10.  PNG encode+decode round-trip: 4x4 RGBA → PNG bytes → decoded dims match
 *  11.  PNG round-trip pixel value: first pixel values preserved
 *  12.  PNG decode of invalid bytes → is_err=1
 *  13.  JPEG encode → is_err=1 with expected message fragment
 *  14.  image_to_grayscale channel=1 identity: grey input stays grey
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/image.h"

static int failures = 0;
static int passes   = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%d]: %s\n", __LINE__, (msg)); \
            failures++; \
        } else { \
            printf("pass [%d]: %s\n", __LINE__, (msg)); \
            passes++; \
        } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

#define ASSERT_STRCONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack),(needle)) != NULL, msg)

/* -------------------------------------------------------------------------
 * Helper: build a small RGBA test image with a predictable pattern.
 * Pixel (x,y) = { x*10, y*10, (x+y)*5, 255 }
 * ------------------------------------------------------------------------- */
static TkImgBuf make_test_rgba(uint32_t w, uint32_t h)
{
    uint64_t len = (uint64_t)w * h * 4;
    uint8_t *data = (uint8_t *)malloc(len);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = data + (y * w + x) * 4;
            p[0] = (uint8_t)(x * 10);
            p[1] = (uint8_t)(y * 10);
            p[2] = (uint8_t)((x + y) * 5);
            p[3] = 255;
        }
    }
    return image_from_raw(data, w, h, 4);
    /* Note: image_from_raw copies the data so we can free the original */
    /* (we intentionally leak the local `data` here to keep test code simple) */
}

/* =========================================================================
 * Test cases
 * ========================================================================= */

/* Test 1: image_from_raw basic construction */
static void test_from_raw(void)
{
    TkImgBuf buf = make_test_rgba(4, 4);
    ASSERT(buf.data     != NULL, "from_raw: data non-null");
    ASSERT(buf.width    == 4,    "from_raw: width==4");
    ASSERT(buf.height   == 4,    "from_raw: height==4");
    ASSERT(buf.channels == 4,    "from_raw: channels==4");
    ASSERT(buf.data_len == 64,   "from_raw: data_len==64");
    image_buf_free(&buf);
}

/* Test 2 & 3: image_pixel_at */
static void test_pixel_at(void)
{
    TkImgBuf buf = make_test_rgba(4, 4);

    /* Happy path: (0,0) */
    PixelResult pr = image_pixel_at(buf, 0, 0);
    ASSERT(pr.is_err == 0,  "pixel_at(0,0): no error");
    ASSERT(pr.ok_len == 4,  "pixel_at(0,0): returns 4 bytes");
    /* pixel(0,0) = {0, 0, 0, 255} per our pattern */
    ASSERT(pr.ok != NULL && pr.ok[0] == 0 && pr.ok[3] == 255,
           "pixel_at(0,0): correct RGBA values");
    free(pr.ok);

    /* Out-of-bounds: x == width */
    PixelResult oob = image_pixel_at(buf, 4, 0);
    ASSERT(oob.is_err == 1, "pixel_at(4,0): is_err==1 for x>=width");

    image_buf_free(&buf);
}

/* Test 4 & 14: image_to_grayscale */
static void test_to_grayscale(void)
{
    TkImgBuf src = make_test_rgba(4, 4);
    TkImgBuf grey = image_to_grayscale(src);

    ASSERT(grey.data     != NULL, "to_grayscale: data non-null");
    ASSERT(grey.channels == 1,    "to_grayscale RGBA→grey: channels==1");
    ASSERT(grey.width    == 4,    "to_grayscale: width preserved");
    ASSERT(grey.height   == 4,    "to_grayscale: height preserved");
    ASSERT(grey.data_len == 16,   "to_grayscale: data_len==16");

    /* Test 14: already-grey input stays channels=1 */
    TkImgBuf grey2 = image_to_grayscale(grey);
    ASSERT(grey2.channels == 1,   "to_grayscale of grey: channels still 1");
    ASSERT(grey2.data_len == 16,  "to_grayscale of grey: data_len preserved");
    image_buf_free(&grey2);

    image_buf_free(&grey);
    image_buf_free(&src);
}

/* Test 5: image_flip_h */
static void test_flip_h(void)
{
    /* 1-row, 2-pixel RGBA image: left pixel = {1,2,3,4}, right = {5,6,7,8} */
    uint8_t raw[8] = {1,2,3,4, 5,6,7,8};
    TkImgBuf buf = image_from_raw(raw, 2, 1, 4);
    TkImgBuf flipped = image_flip_h(buf);

    ASSERT(flipped.data != NULL, "flip_h: data non-null");
    /* After flip, pixel(0,0) should be old pixel(1,0) = {5,6,7,8} */
    ASSERT(flipped.data[0] == 5 && flipped.data[1] == 6 &&
           flipped.data[2] == 7 && flipped.data[3] == 8,
           "flip_h: first pixel is now the former last pixel");
    /* And pixel(1,0) should be {1,2,3,4} */
    ASSERT(flipped.data[4] == 1 && flipped.data[5] == 2,
           "flip_h: last pixel is now the former first pixel");

    image_buf_free(&flipped);
    image_buf_free(&buf);
}

/* Test 6: image_flip_v */
static void test_flip_v(void)
{
    /* 2-row, 1-pixel RGBA image: top = {1,2,3,4}, bottom = {5,6,7,8} */
    uint8_t raw[8] = {1,2,3,4, 5,6,7,8};
    TkImgBuf buf = image_from_raw(raw, 1, 2, 4);
    TkImgBuf flipped = image_flip_v(buf);

    ASSERT(flipped.data != NULL, "flip_v: data non-null");
    /* After flip, row 0 should be old row 1 = {5,6,7,8} */
    ASSERT(flipped.data[0] == 5 && flipped.data[1] == 6 &&
           flipped.data[2] == 7 && flipped.data[3] == 8,
           "flip_v: top row is now the former bottom row");
    /* And row 1 should be {1,2,3,4} */
    ASSERT(flipped.data[4] == 1 && flipped.data[5] == 2,
           "flip_v: bottom row is now the former top row");

    image_buf_free(&flipped);
    image_buf_free(&buf);
}

/* Test 7: image_resize */
static void test_resize(void)
{
    TkImgBuf src = make_test_rgba(4, 4);
    TkImgBuf dst = image_resize(src, 2, 2);

    ASSERT(dst.data     != NULL, "resize 4x4→2x2: data non-null");
    ASSERT(dst.width    == 2,    "resize 4x4→2x2: width==2");
    ASSERT(dst.height   == 2,    "resize 4x4→2x2: height==2");
    ASSERT(dst.channels == 4,    "resize 4x4→2x2: channels preserved");
    ASSERT(dst.data_len == 16,   "resize 4x4→2x2: data_len==16");

    image_buf_free(&dst);
    image_buf_free(&src);
}

/* Test 8 & 9: image_crop */
static void test_crop(void)
{
    TkImgBuf src = make_test_rgba(4, 4);

    /* Valid crop: (1,1,2,2) → 2x2 */
    ImgResult r = image_crop(src, 1, 1, 2, 2);
    ASSERT(r.is_err  == 0, "crop(1,1,2,2): no error");
    ASSERT(r.ok.width  == 2, "crop(1,1,2,2): width==2");
    ASSERT(r.ok.height == 2, "crop(1,1,2,2): height==2");
    ASSERT(r.ok.channels == 4, "crop(1,1,2,2): channels==4");
    /* First pixel of crop should be src pixel (1,1) = {10, 10, 10, 255} */
    ASSERT(r.ok.data != NULL && r.ok.data[0] == 10,
           "crop(1,1,2,2): first pixel R matches src(1,1)");
    image_buf_free(&r.ok);

    /* Out-of-bounds crop: x+w (3+2=5) > width (4) */
    ImgResult bad = image_crop(src, 3, 0, 2, 1);
    ASSERT(bad.is_err == 1, "crop OOB: is_err==1 when x+w > width");

    image_buf_free(&src);
}

/* Tests 10 & 11: PNG encode+decode round-trip */
static void test_png_roundtrip(void)
{
    TkImgBuf src = make_test_rgba(4, 4);

    /* Encode */
    ImgEncResult enc = image_encode(src, IMG_FMT_PNG, 0);
    ASSERT(enc.is_err == 0,    "png_encode: no error");
    ASSERT(enc.ok     != NULL, "png_encode: output non-null");
    ASSERT(enc.ok_len  > 0,   "png_encode: output non-empty");

    /* Decode */
    ImgResult dec = image_decode(enc.ok, enc.ok_len);
    ASSERT(dec.is_err    == 0, "png_decode after encode: no error");
    ASSERT(dec.ok.width  == 4, "png_roundtrip: width==4");
    ASSERT(dec.ok.height == 4, "png_roundtrip: height==4");
    ASSERT(dec.ok.channels == 4, "png_roundtrip: channels==4");

    /* Test 11: first pixel should match: src(0,0) = {0,0,0,255} */
    ASSERT(dec.ok.data != NULL && dec.ok.data[0] == 0 &&
           dec.ok.data[1] == 0 && dec.ok.data[2] == 0 &&
           dec.ok.data[3] == 255,
           "png_roundtrip: first pixel values preserved");

    image_buf_free(&dec.ok);
    free(enc.ok);
    image_buf_free(&src);
}

/* Test 12: PNG decode of invalid bytes */
static void test_png_decode_invalid(void)
{
    uint8_t garbage[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                           0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
    ImgResult res = image_decode(garbage, sizeof(garbage));
    ASSERT(res.is_err == 1,    "decode invalid bytes: is_err==1");
    ASSERT(res.err_msg != NULL, "decode invalid bytes: err_msg non-null");
}

/* Test 13: JPEG encode stub */
static void test_jpeg_encode_stub(void)
{
    TkImgBuf src = make_test_rgba(2, 2);
    ImgEncResult res = image_encode(src, IMG_FMT_JPEG, 85);
    ASSERT(res.is_err == 1, "jpeg_encode: is_err==1 (stubbed)");
    ASSERT_STRCONTAINS(res.err_msg, "libjpeg",
                       "jpeg_encode: err_msg mentions libjpeg");
    image_buf_free(&src);
}

/* =========================================================================
 * Story 34.3.1 — transforms and filters test cases
 * ========================================================================= */

/* Helper: make a flat gray image (all pixels set to `val`) */
static TkImgBuf make_gray_rgb(uint32_t w, uint32_t h, uint8_t val)
{
    uint64_t len = (uint64_t)w * h * 3;
    uint8_t *raw = (uint8_t *)malloc(len);
    memset(raw, val, len);
    TkImgBuf b = image_from_raw(raw, w, h, 3);
    free(raw);
    return b;
}

/* Test: image_brightness — brighten a gray image */
static void test_brightness_brighter(void)
{
    /* All pixels = 100 (RGB) */
    TkImgBuf src = make_gray_rgb(4, 4, 100);
    TkImgBuf out = image_brightness(src, 2.0);

    ASSERT(out.data != NULL, "brightness(2.0): data non-null");
    ASSERT(out.width == 4 && out.height == 4, "brightness(2.0): dims preserved");
    /* 100 * 2.0 = 200, expect 200 */
    ASSERT(out.data[0] == 200, "brightness(2.0): pixel value doubled");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* Test: image_brightness factor=0.0 → all pixels are 0 */
static void test_brightness_zero(void)
{
    TkImgBuf src = make_gray_rgb(4, 4, 128);
    TkImgBuf out = image_brightness(src, 0.0);

    ASSERT(out.data != NULL, "brightness(0.0): data non-null");
    ASSERT(out.data[0] == 0, "brightness(0.0): pixels are 0");
    ASSERT(out.data[5] == 0, "brightness(0.0): pixels are 0 (mid-buffer)");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* Test: image_contrast(buf, 2.0) → pixels move away from 128 */
static void test_contrast(void)
{
    /* Pixel value 200: should become 128 + (200-128)*2 = 272 → clamped to 255 */
    TkImgBuf src = make_gray_rgb(4, 4, 200);
    TkImgBuf out = image_contrast(src, 2.0);

    ASSERT(out.data != NULL, "contrast(2.0): data non-null");
    ASSERT(out.data[0] == 255, "contrast(2.0): high pixel clamped to 255");

    image_buf_free(&out);
    image_buf_free(&src);

    /* Pixel value 64: 128 + (64-128)*2 = 0 (or below → clamped to 0) */
    TkImgBuf src2 = make_gray_rgb(4, 4, 64);
    TkImgBuf out2 = image_contrast(src2, 2.0);

    ASSERT(out2.data != NULL, "contrast(2.0) low: data non-null");
    ASSERT(out2.data[0] == 0, "contrast(2.0): low pixel clamped to 0");

    image_buf_free(&out2);
    image_buf_free(&src2);
}

/* Test: image_sharpen — non-null result, same dimensions */
static void test_sharpen(void)
{
    TkImgBuf src = make_test_rgba(8, 8);
    TkImgBuf out = image_sharpen(src);

    ASSERT(out.data     != NULL, "sharpen: data non-null");
    ASSERT(out.width    == 8,    "sharpen: width preserved");
    ASSERT(out.height   == 8,    "sharpen: height preserved");
    ASSERT(out.channels == 4,    "sharpen: channels preserved");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* Test: image_blur(buf, 1) — non-null result, same dimensions */
static void test_blur(void)
{
    TkImgBuf src = make_test_rgba(8, 8);
    TkImgBuf out = image_blur(src, 1);

    ASSERT(out.data     != NULL, "blur(1): data non-null");
    ASSERT(out.width    == 8,    "blur(1): width preserved");
    ASSERT(out.height   == 8,    "blur(1): height preserved");
    ASSERT(out.channels == 4,    "blur(1): channels preserved");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* Test: image_rotate(buf, 90.0) — same dimensions */
static void test_rotate(void)
{
    TkImgBuf src = make_test_rgba(8, 8);
    TkImgBuf out = image_rotate(src, 90.0);

    ASSERT(out.data     != NULL, "rotate(90): data non-null");
    ASSERT(out.width    == 8,    "rotate(90): width preserved");
    ASSERT(out.height   == 8,    "rotate(90): height preserved");
    ASSERT(out.channels == 4,    "rotate(90): channels preserved");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* Test: image_paste — paste 2x2 src at (1,1) on 4x4 dst */
static void test_paste(void)
{
    /* dst: 4x4 RGBA all zeros */
    uint64_t dst_len = (uint64_t)4 * 4 * 4;
    uint8_t *dst_raw = (uint8_t *)malloc(dst_len);
    memset(dst_raw, 0, dst_len);
    TkImgBuf dst = image_from_raw(dst_raw, 4, 4, 4);
    free(dst_raw);

    /* src: 2x2 RGBA, all pixels = {50, 100, 150, 255} */
    uint64_t src_len = (uint64_t)2 * 2 * 4;
    uint8_t *src_raw = (uint8_t *)malloc(src_len);
    for (int i = 0; i < 4; i++) {
        src_raw[i*4+0] = 50;
        src_raw[i*4+1] = 100;
        src_raw[i*4+2] = 150;
        src_raw[i*4+3] = 255;
    }
    TkImgBuf src = image_from_raw(src_raw, 2, 2, 4);
    free(src_raw);

    TkImgBuf out = image_paste(dst, src, 1, 1);

    ASSERT(out.data  != NULL, "paste: data non-null");
    ASSERT(out.width == 4,    "paste: dst width preserved");
    ASSERT(out.height== 4,    "paste: dst height preserved");

    /* Pixel at (1,1) in out should be src pixel: {50,100,150,255}
     * Alpha=255 so out = 1*src + 0*dst = src */
    uint64_t idx = ((uint64_t)1 * 4 + 1) * 4; /* y=1, x=1, stride=4*4=16 */
    ASSERT(out.data[idx+0] == 50,  "paste: pixel(1,1) R == 50");
    ASSERT(out.data[idx+1] == 100, "paste: pixel(1,1) G == 100");
    ASSERT(out.data[idx+2] == 150, "paste: pixel(1,1) B == 150");

    /* Pixel at (0,0) should still be 0 (outside paste area) */
    ASSERT(out.data[0] == 0, "paste: pixel(0,0) unchanged (R==0)");

    image_buf_free(&out);
    image_buf_free(&src);
    image_buf_free(&dst);
}

/* =========================================================================
 * Story 34.3.2 tests: histogram, quantize, text_draw
 * ========================================================================= */

/* Test histogram: 2x2 RGBA image with 4 known pixel colors.
 * Pixels: (255,0,0,255), (0,255,0,255), (0,0,255,255), (128,128,128,255) */
static void test_histogram(void)
{
    uint8_t raw[16] = {
        255,   0,   0, 255,   /* pixel (0,0): red */
          0, 255,   0, 255,   /* pixel (1,0): green */
          0,   0, 255, 255,   /* pixel (0,1): blue */
        128, 128, 128, 255    /* pixel (1,1): grey */
    };
    TkImgBuf buf = image_from_raw(raw, 2, 2, 4);
    ImgHistogram h = image_histogram(buf);

    /* Red channel: one pixel at 255, one at 128, two at 0 */
    ASSERT(h.r[255] == 1, "histogram: r[255]==1 (red pixel)");
    ASSERT(h.r[128] == 1, "histogram: r[128]==1 (grey pixel)");
    ASSERT(h.r[0]   == 2, "histogram: r[0]==2 (green+blue pixels)");

    /* Green channel: one pixel at 255, one at 128, two at 0 */
    ASSERT(h.g[255] == 1, "histogram: g[255]==1 (green pixel)");
    ASSERT(h.g[128] == 1, "histogram: g[128]==1 (grey pixel)");
    ASSERT(h.g[0]   == 2, "histogram: g[0]==2 (red+blue pixels)");

    /* Blue channel: one pixel at 255, one at 128, two at 0 */
    ASSERT(h.b[255] == 1, "histogram: b[255]==1 (blue pixel)");
    ASSERT(h.b[128] == 1, "histogram: b[128]==1 (grey pixel)");
    ASSERT(h.b[0]   == 2, "histogram: b[0]==2 (red+green pixels)");

    /* Alpha channel: all four pixels have alpha=255 */
    ASSERT(h.a[255] == 4, "histogram: a[255]==4 (all opaque)");

    image_buf_free(&buf);
}

/* Test quantize: 8x8 image with exactly 2 alternating colors.
 * Quantize to 2 → every output pixel must map to one of those 2 colors. */
static void test_quantize(void)
{
    uint8_t color_a[4] = {200,  50,  50, 255};
    uint8_t color_b[4] = { 50, 200,  50, 255};
    uint32_t w = 8, h = 8;
    uint8_t raw[8 * 8 * 4];
    uint32_t i;
    for (i = 0; i < w * h; i++) {
        uint8_t *p = raw + i * 4;
        const uint8_t *src = (i % 2 == 0) ? color_a : color_b;
        p[0] = src[0]; p[1] = src[1]; p[2] = src[2]; p[3] = src[3];
    }
    TkImgBuf src = image_from_raw(raw, w, h, 4);
    TkImgBuf out = image_quantize(src, 2);

    ASSERT(out.data     != NULL, "quantize 2colors->2: data non-null");
    ASSERT(out.width    == w,    "quantize 2colors->2: width preserved");
    ASSERT(out.height   == h,    "quantize 2colors->2: height preserved");
    ASSERT(out.channels == 4,    "quantize 2colors->2: channels preserved");

    /* Every pixel must be close to one of the two original colors. */
    int bad = 0;
    if (out.data) {
        for (i = 0; i < w * h; i++) {
            const uint8_t *p = out.data + i * 4;
            int da0 = (int)p[0]-200; int da1 = (int)p[1]-50;  int da2 = (int)p[2]-50;
            int db0 = (int)p[0]-50;  int db1 = (int)p[1]-200; int db2 = (int)p[2]-50;
            int is_a = (da0*da0 + da1*da1 + da2*da2) < (30*30*3);
            int is_b = (db0*db0 + db1*db1 + db2*db2) < (30*30*3);
            if (!is_a && !is_b) bad++;
        }
    }
    ASSERT(bad == 0, "quantize 2colors->2: all pixels near one of 2 colors");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* Test text_draw: draw "A" on an 80x16 white RGBA image.
 * Result must have same dimensions and contain at least one non-white pixel. */
static void test_text_draw(void)
{
    uint32_t w = 80, h = 16;
    uint64_t len = (uint64_t)w * h * 4;
    uint8_t *raw = (uint8_t *)malloc(len);
    memset(raw, 255, len);
    TkImgBuf src = image_from_raw(raw, w, h, 4);
    free(raw);

    /* Draw black "A" at (2,2) size=1 color=0x000000FF */
    TkImgBuf out = image_text_draw(src, "A", 2, 2, 1, 0x000000FFu);

    ASSERT(out.data     != NULL, "text_draw 'A': result non-null");
    ASSERT(out.width    == w,    "text_draw 'A': width preserved");
    ASSERT(out.height   == h,    "text_draw 'A': height preserved");
    ASSERT(out.channels == 4,    "text_draw 'A': channels preserved");

    /* At least one pixel must be non-white (drawing happened). */
    int found_nonwhite = 0;
    if (out.data) {
        uint64_t idx;
        for (idx = 0; idx < (uint64_t)w * h * 4; idx += 4) {
            if (out.data[idx] != 255 || out.data[idx+1] != 255 ||
                out.data[idx+2] != 255) {
                found_nonwhite = 1;
                break;
            }
        }
    }
    ASSERT(found_nonwhite == 1, "text_draw 'A': at least one non-white pixel drawn");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* =========================================================================
 * main
 * ========================================================================= */

/* =========================================================================
 * Story 135.5 — adaptive threshold, convolution, TIFF.
 *
 * Every assertion below is on PIXEL VALUES.  A dimensions-only check (which
 * is what the pre-existing test_rotate was) passes on a function that returns
 * its input untouched, and that is the failure mode these functions are most
 * likely to have.
 * ========================================================================= */

/* ---- 1. adaptive threshold ---------------------------------------------- */

/*
 * The point of Sauvola is that it survives uneven illumination, so the
 * fixture is a page lit from one side: the background ramps from 40 on the
 * left to 220 on the right, and two ink bars sit at x=8..12 and x=50..54,
 * each 60% of the LOCAL background.
 *
 * That makes the fixture adversarial to any global threshold by construction:
 * the ink on the bright side (about 113) is lighter than the paper on the
 * dark side (about 40), so no single cut separates ink from paper.  The test
 * asserts that impossibility first — otherwise a global threshold would pass
 * this test and it would prove nothing about adaptivity.
 */
#define AT_W 64
#define AT_H 32

static int at_bg(uint32_t x) { return 40 + (int)((x * 180) / (AT_W - 1)); }
static int at_is_ink(uint32_t x, uint32_t y)
{
    return (y >= 8 && y <= 24) && ((x >= 8 && x <= 12) || (x >= 50 && x <= 54));
}

static void test_adaptive_threshold(void)
{
    uint8_t *raw = (uint8_t *)malloc(AT_W * AT_H);
    for (uint32_t y = 0; y < AT_H; y++)
        for (uint32_t x = 0; x < AT_W; x++)
            raw[y * AT_W + x] = (uint8_t)(at_is_ink(x, y)
                                          ? (at_bg(x) * 6) / 10 : at_bg(x));
    TkImgBuf src = image_from_raw(raw, AT_W, AT_H, 1);
    free(raw);

    /* -- the control: prove NO global threshold can succeed here -- */
    int darkest_paper = 255, brightest_ink = 0;
    for (uint32_t y = 0; y < AT_H; y++)
        for (uint32_t x = 0; x < AT_W; x++) {
            int v = src.data[y * AT_W + x];
            if (at_is_ink(x, y)) { if (v > brightest_ink)  brightest_ink  = v; }
            else                 { if (v < darkest_paper)  darkest_paper  = v; }
        }
    ASSERT(brightest_ink > darkest_paper,
           "adaptivethreshold fixture: ink on the lit side is brighter than "
           "paper on the dark side, so no global threshold can separate them");

    ImgResult r = image_adaptive_threshold(src, 15, 0.2);
    ASSERT(!r.is_err, "adaptivethreshold: 64x32 ramp succeeds");
    if (r.is_err) { image_buf_free(&src); return; }

    ASSERT(r.ok.channels == 1, "adaptivethreshold: result is single-channel");
    ASSERT(r.ok.width == AT_W && r.ok.height == AT_H,
           "adaptivethreshold: dimensions preserved");

    int only_binary = 1;
    for (uint64_t i = 0; i < r.ok.data_len; i++)
        if (r.ok.data[i] != 0 && r.ok.data[i] != 255) only_binary = 0;
    ASSERT(only_binary, "adaptivethreshold: every pixel is exactly 0 or 255");

    /* Ink must come out black on BOTH sides of the illumination ramp. */
    int ink_bad = 0;
    for (uint32_t y = 12; y <= 20; y++) {
        if (r.ok.data[y * AT_W + 10] != 0) ink_bad++;
        if (r.ok.data[y * AT_W + 52] != 0) ink_bad++;
    }
    ASSERT(ink_bad == 0,
           "adaptivethreshold: ink is black at x=10 (dark side) AND x=52 "
           "(lit side) — the case a global threshold cannot do");

    /* Paper well away from the bars must come out white, on both sides. */
    int paper_bad = 0;
    for (uint32_t y = 4; y < AT_H - 4; y++)
        for (uint32_t x = 22; x <= 42; x++)
            if (r.ok.data[y * AT_W + x] != 255) paper_bad++;
    ASSERT(paper_bad == 0,
           "adaptivethreshold: blank paper between the bars stays white "
           "(Sauvola's variance term is what stops it speckling)");

    /* And the result is not the input: a passthrough would keep the ramp. */
    ASSERT(r.ok.data[16 * AT_W + 30] != src.data[16 * AT_W + 30],
           "adaptivethreshold: output differs from input (not a passthrough)");

    image_buf_free(&r.ok);

    /* window 0 selects the default, and an even window is made odd. */
    ImgResult d = image_adaptive_threshold(src, 0, 0.2);
    ASSERT(!d.is_err && d.ok.data != NULL,
           "adaptivethreshold: window 0 selects the default window");
    image_buf_free(&d.ok);

    ImgResult e = image_adaptive_threshold(src, 15, 99.0);
    ASSERT(e.is_err, "adaptivethreshold: k out of range is an error");

    image_buf_free(&src);
}

/* ---- 2. convolution ------------------------------------------------------ */

static void test_convolve(void)
{
    /* A single bright pixel at the centre of a 5x5 black field: the response
     * of any kernel to an impulse is the kernel itself, which is the
     * strongest statement available about a convolution. */
    uint8_t *raw = (uint8_t *)calloc(25, 1);
    raw[2 * 5 + 2] = 255;
    TkImgBuf src = image_from_raw(raw, 5, 5, 1);
    free(raw);

    /* Identity kernel: output must equal input, byte for byte. */
    double ident[9] = {0,0,0, 0,1,0, 0,0,0};
    ImgResult id = image_convolve(src, ident, 3, 1.0, 0.0);
    ASSERT(!id.is_err, "convolve: identity kernel succeeds");
    if (!id.is_err) {
        ASSERT(memcmp(id.ok.data, src.data, 25) == 0,
               "convolve: identity kernel reproduces the input exactly");
        image_buf_free(&id.ok);
    }

    /* Box blur, divisor 0 => normalise by the kernel sum (9).
     * 255/9 = 28.33, rounded to 28, over exactly the 3x3 neighbourhood. */
    double box[9] = {1,1,1, 1,1,1, 1,1,1};
    ImgResult b = image_convolve(src, box, 3, 0.0, 0.0);
    ASSERT(!b.is_err, "convolve: box kernel with divisor 0 succeeds");
    if (!b.is_err) {
        int wrong = 0;
        for (uint32_t y = 0; y < 5; y++)
            for (uint32_t x = 0; x < 5; x++) {
                int want = (x >= 1 && x <= 3 && y >= 1 && y <= 3) ? 28 : 0;
                if (b.ok.data[y * 5 + x] != want) wrong++;
            }
        ASSERT(wrong == 0,
               "convolve: impulse response is the kernel — 28 over the 3x3 "
               "neighbourhood, 0 everywhere else (divisor 0 took the sum, 9)");
        image_buf_free(&b.ok);
    }

    /* A zero-sum kernel must NOT be divided by zero: divisor 0 leaves it
     * unscaled.  Sobel-x over a vertical step edge, offset 128. */
    uint8_t *edge = (uint8_t *)malloc(25);
    for (uint32_t y = 0; y < 5; y++)
        for (uint32_t x = 0; x < 5; x++) edge[y * 5 + x] = (x < 2) ? 0 : 200;
    TkImgBuf es = image_from_raw(edge, 5, 5, 1);
    free(edge);

    double sobel[9] = {-1,0,1, -2,0,2, -1,0,1};
    ImgResult s = image_convolve(es, sobel, 3, 0.0, 128.0);
    ASSERT(!s.is_err, "convolve: zero-sum kernel is not divided by zero");
    if (!s.is_err) {
        /* At x=1 the window straddles the step: columns 0,1 are 0 and
         * column 2 is 200, so the response is 4*200 = 800, clamped to 255. */
        ASSERT(s.ok.data[2 * 5 + 1] == 255,
               "convolve: Sobel-x saturates on the step edge at x=1");
        /* At x=4 (flat, clamp-to-edge) the response is 0, so offset shows. */
        ASSERT(s.ok.data[2 * 5 + 4] == 128,
               "convolve: flat region returns the offset (128), so offset is "
               "applied after the divisor");
        image_buf_free(&s.ok);
    }
    image_buf_free(&es);

    ImgResult bad = image_convolve(src, box, 2, 1.0, 0.0);
    ASSERT(bad.is_err, "convolve: an even kernel size is an error");

    image_buf_free(&src);
}

/* ---- 3. rotation, on pixel values --------------------------------------- */

static void test_rotate_pixels(void)
{
    /* make_test_rgba: pixel (X,Y) = {X*10, Y*10, (X+Y)*5, 255}, so adjacent
     * pixels differ by 10 per channel and a +-1 tolerance still pins down
     * exactly which source pixel was sampled. */
    TkImgBuf src = make_test_rgba(8, 8);
    TkImgBuf out = image_rotate(src, 180.0);
    ASSERT(out.data != NULL, "rotate(180): produced a buffer");
    if (!out.data) { image_buf_free(&src); return; }

    struct { uint32_t ox, oy, sx, sy; } cases[3] = {
        {3, 3, 5, 5}, {2, 5, 6, 3}, {5, 2, 3, 6}
    };
    int wrong = 0;
    for (int i = 0; i < 3; i++) {
        const uint8_t *p = out.data + ((uint64_t)cases[i].oy * 8 + cases[i].ox) * 4;
        int wr = (int)(cases[i].sx * 10), wg = (int)(cases[i].sy * 10);
        if (abs((int)p[0] - wr) > 1 || abs((int)p[1] - wg) > 1) wrong++;
    }
    ASSERT(wrong == 0,
           "rotate(180): each output pixel carries the value of the source "
           "pixel opposite the centre (bilinear inverse mapping)");

    /* A passthrough would leave (2,5) at R=20; the rotation puts R=60 there. */
    ASSERT(abs((int)out.data[(5 * 8 + 2) * 4] - 60) <= 1,
           "rotate(180): output is not the input");

    image_buf_free(&out);
    image_buf_free(&src);
}

/* ---- 4. TIFF, including multi-page -------------------------------------- */

typedef struct { uint8_t *b; uint64_t len, cap; } TBuf;

static void tb_put(TBuf *t, const void *src, uint64_t n)
{
    if (t->len + n > t->cap) {
        t->cap = (t->len + n) * 2 + 64;
        t->b = (uint8_t *)realloc(t->b, (size_t)t->cap);
    }
    memcpy(t->b + t->len, src, (size_t)n);
    t->len += n;
}
static void tb_u16(TBuf *t, uint16_t v) { uint8_t q[2] = {(uint8_t)(v & 0xFF), (uint8_t)(v >> 8)}; tb_put(t, q, 2); }
static void tb_u32(TBuf *t, uint32_t v) { uint8_t q[4] = {(uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; tb_put(t, q, 4); }
static void tb_patch32(TBuf *t, uint64_t at, uint32_t v)
{
    t->b[at] = (uint8_t)v; t->b[at+1] = (uint8_t)(v >> 8);
    t->b[at+2] = (uint8_t)(v >> 16); t->b[at+3] = (uint8_t)(v >> 24);
}

typedef struct {
    uint32_t w, h, bps, spp, photo, comp;
    const uint8_t *strip;
    uint32_t striplen;
} TPage;

/* A deliberately small little-endian TIFF writer, so the multi-page fixture
 * comes from this test rather than from the decoder under test. */
static uint8_t *tiff_build(const TPage *pages, int np, uint64_t *out_len)
{
    TBuf t; memset(&t, 0, sizeof t);
    tb_put(&t, "II", 2); tb_u16(&t, 42); tb_u32(&t, 0);   /* first-IFD slot */

    uint64_t *bpsoff = (uint64_t *)calloc((size_t)np, sizeof(uint64_t));
    uint64_t *stroff = (uint64_t *)calloc((size_t)np, sizeof(uint64_t));
    for (int i = 0; i < np; i++) {
        if (pages[i].spp > 1) {
            bpsoff[i] = t.len;
            for (uint32_t s = 0; s < pages[i].spp; s++) tb_u16(&t, (uint16_t)pages[i].bps);
        }
        stroff[i] = t.len;
        tb_put(&t, pages[i].strip, pages[i].striplen);
    }

    uint64_t prev_next = 4;                 /* where the next-IFD pointer goes */
    for (int i = 0; i < np; i++) {
        tb_patch32(&t, prev_next, (uint32_t)t.len);
        tb_u16(&t, 9);                      /* nine directory entries */
        #define ENT(tag, ty, cnt, val) do { \
            tb_u16(&t, (uint16_t)(tag)); tb_u16(&t, (uint16_t)(ty)); \
            tb_u32(&t, (uint32_t)(cnt)); \
            if ((ty) == 3 && (cnt) == 1) { tb_u16(&t, (uint16_t)(val)); tb_u16(&t, 0); } \
            else tb_u32(&t, (uint32_t)(val)); \
        } while (0)
        ENT(256, 4, 1, pages[i].w);                       /* ImageWidth */
        ENT(257, 4, 1, pages[i].h);                       /* ImageLength */
        if (pages[i].spp > 1) ENT(258, 3, pages[i].spp, bpsoff[i]);
        else                  ENT(258, 3, 1, pages[i].bps);
        ENT(259, 3, 1, pages[i].comp);                    /* Compression */
        ENT(262, 3, 1, pages[i].photo);                   /* Photometric */
        ENT(273, 4, 1, stroff[i]);                        /* StripOffsets */
        ENT(277, 3, 1, pages[i].spp);                     /* SamplesPerPixel */
        ENT(278, 4, 1, pages[i].h);                       /* RowsPerStrip */
        ENT(279, 4, 1, pages[i].striplen);                /* StripByteCounts */
        #undef ENT
        prev_next = t.len;
        tb_u32(&t, 0);
    }
    free(bpsoff); free(stroff);
    *out_len = t.len;
    return t.b;
}

/* PackBits encoder: runs of three or more become a repeat, everything else a
 * literal.  Both branches of the decoder are therefore exercised. */
static uint32_t packbits_encode(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t run = 1;
        while (i + run < n && run < 128 && src[i + run] == src[i]) run++;
        if (run >= 3) {
            dst[o++] = (uint8_t)(int8_t)(1 - (int)run);
            dst[o++] = src[i];
            i += run;
        } else {
            uint32_t lit = 0;
            while (i + lit < n && lit < 128) {
                uint32_t r2 = 1;
                while (i + lit + r2 < n && r2 < 4 && src[i + lit + r2] == src[i + lit]) r2++;
                if (r2 >= 3) break;
                lit++;
            }
            if (lit == 0) lit = 1;
            dst[o++] = (uint8_t)(lit - 1);
            memcpy(dst + o, src + i, lit);
            o += lit; i += lit;
        }
    }
    return o;
}

static void test_tiff_multipage(void)
{
    /* page 0: 2x2 8-bit grey, BlackIsZero, uncompressed */
    static const uint8_t g[4] = {0, 64, 128, 255};
    /* page 1: 3x1 RGB, uncompressed */
    static const uint8_t rgb[9] = {255,0,0, 0,255,0, 0,0,255};

    TPage pages[2];
    pages[0].w = 2; pages[0].h = 2; pages[0].bps = 8; pages[0].spp = 1;
    pages[0].photo = 1; pages[0].comp = 1; pages[0].strip = g; pages[0].striplen = 4;
    pages[1].w = 3; pages[1].h = 1; pages[1].bps = 8; pages[1].spp = 3;
    pages[1].photo = 2; pages[1].comp = 1; pages[1].strip = rgb; pages[1].striplen = 9;

    uint64_t len = 0;
    uint8_t *tif = tiff_build(pages, 2, &len);

    ASSERT(image_tiff_pages(tif, len) == 2,
           "tiffpages: a two-IFD TIFF reports 2 pages (multi-page is the "
           "part that is easy to leave out)");

    ImgResult p0 = image_tiff_decode(tif, len, 0);
    ASSERT(!p0.is_err, "tiffdecode: page 0 decodes");
    if (!p0.is_err) {
        ASSERT(p0.ok.width == 2 && p0.ok.height == 2 && p0.ok.channels == 1,
               "tiffdecode: page 0 is 2x2 single-channel");
        ASSERT(memcmp(p0.ok.data, g, 4) == 0,
               "tiffdecode: page 0 pixels are exactly {0, 64, 128, 255}");
        image_buf_free(&p0.ok);
    }

    ImgResult p1 = image_tiff_decode(tif, len, 1);
    ASSERT(!p1.is_err, "tiffdecode: page 1 decodes");
    if (!p1.is_err) {
        ASSERT(p1.ok.width == 3 && p1.ok.height == 1 && p1.ok.channels == 3,
               "tiffdecode: page 1 is 3x1 RGB — a SECOND page with different "
               "dimensions, so page 0 was not returned twice");
        ASSERT(memcmp(p1.ok.data, rgb, 9) == 0,
               "tiffdecode: page 1 pixels are exactly red, green, blue");
        image_buf_free(&p1.ok);
    }

    ImgResult p2 = image_tiff_decode(tif, len, 2);
    ASSERT(p2.is_err, "tiffdecode: page 2 of a 2-page file is an error");

    /* image.decode auto-detects TIFF and yields page 0. */
    ImgResult ad = image_decode(tif, len);
    ASSERT(!ad.is_err && ad.ok.width == 2 && ad.ok.height == 2 &&
           memcmp(ad.ok.data, g, 4) == 0,
           "decode: a TIFF buffer is auto-detected and yields page 0");
    if (!ad.is_err) image_buf_free(&ad.ok);

    free(tif);
}

static void test_tiff_packbits_and_bilevel(void)
{
    /* 8x2 grey with a long run and some literals, PackBits-compressed. */
    uint8_t raw[16];
    for (int i = 0; i < 16; i++) raw[i] = (i < 6) ? 77 : (uint8_t)(i * 7);
    uint8_t enc[64];
    uint32_t enclen = packbits_encode(raw, 16, enc);
    ASSERT(enclen < 16, "packbits fixture: the run actually compressed");

    TPage p;
    p.w = 8; p.h = 2; p.bps = 8; p.spp = 1; p.photo = 1; p.comp = 32773;
    p.strip = enc; p.striplen = enclen;
    uint64_t len = 0;
    uint8_t *tif = tiff_build(&p, 1, &len);

    ImgResult r = image_tiff_decode(tif, len, 0);
    ASSERT(!r.is_err, "tiffdecode: PackBits page decodes");
    if (!r.is_err) {
        ASSERT(memcmp(r.ok.data, raw, 16) == 0,
               "tiffdecode: PackBits round-trips every one of the 16 pixels");
        image_buf_free(&r.ok);
    }
    free(tif);

    /* Bilevel, WhiteIsZero: bit 1 means ink.  0xB2 = 1011 0010. */
    static const uint8_t bits[1] = {0xB2};
    TPage bp;
    bp.w = 8; bp.h = 1; bp.bps = 1; bp.spp = 1; bp.photo = 0; bp.comp = 1;
    bp.strip = bits; bp.striplen = 1;
    uint64_t blen = 0;
    uint8_t *btif = tiff_build(&bp, 1, &blen);

    ImgResult b = image_tiff_decode(btif, blen, 0);
    ASSERT(!b.is_err, "tiffdecode: 1-bit WhiteIsZero page decodes");
    if (!b.is_err) {
        static const uint8_t want[8] = {0, 255, 0, 0, 255, 255, 0, 255};
        ASSERT(b.ok.width == 8 && b.ok.channels == 1 &&
               memcmp(b.ok.data, want, 8) == 0,
               "tiffdecode: 0xB2 as WhiteIsZero bilevel expands to "
               "black/white/black/black/white/white/black/white");
        image_buf_free(&b.ok);
    }
    free(btif);

    /* A PNG is not a TIFF, and must be reported as such rather than guessed. */
    static const uint8_t png_sig[8] = {137,80,78,71,13,10,26,10};
    ASSERT(image_tiff_pages(png_sig, 8) == -1,
           "tiffpages: a PNG signature reports -1, not a page count");
}

int main(void)
{
    printf("=== test_image (Story 18.1.6) ===\n\n");

    test_from_raw();
    test_pixel_at();
    test_to_grayscale();
    test_flip_h();
    test_flip_v();
    test_resize();
    test_crop();
    test_png_roundtrip();
    test_png_decode_invalid();
    test_jpeg_encode_stub();

    printf("\n=== Story 34.3.1: transforms and filters ===\n\n");

    test_brightness_brighter();
    test_brightness_zero();
    test_contrast();
    test_sharpen();
    test_blur();
    test_rotate();
    test_paste();

    printf("\n=== Story 34.3.2: histogram, quantize, text_draw ===\n\n");

    test_histogram();
    test_quantize();
    test_text_draw();

    printf("\n=== Story 135.5: adaptive threshold, convolution, TIFF ===\n\n");

    test_adaptive_threshold();
    test_convolve();
    test_rotate_pixels();
    test_tiff_multipage();
    test_tiff_packbits_and_bilevel();

    printf("\n=== Results: %d passed, %d failed ===\n", passes, failures);
    return failures ? 1 : 0;
}
