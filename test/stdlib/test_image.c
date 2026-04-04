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

    printf("\n=== Results: %d passed, %d failed ===\n", passes, failures);
    return failures ? 1 : 0;
}
