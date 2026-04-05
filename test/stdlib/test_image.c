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

    printf("\n=== Results: %d passed, %d failed ===\n", passes, failures);
    return failures ? 1 : 0;
}
