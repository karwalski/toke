/*
 * test_image_pipeline_integration.c — Integration tests for the image
 * processing pipeline: create -> resize -> grayscale -> encode -> decode.
 *
 * Story: 21.1.5
 *
 * Build:
 *   $(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -o test_image_pipeline_integration \
 *       test_image_pipeline_integration.c ../../src/stdlib/image.c
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

/* -----------------------------------------------------------------------
 * Helper: build a WxH RGBA buffer with a gradient pattern.
 *   R increases left-to-right:  R = x * 255 / (w-1)
 *   G increases top-to-bottom:  G = y * 255 / (h-1)
 *   B = 128 constant, A = 255
 * For 1x1 images, R=G=0.
 * ----------------------------------------------------------------------- */
static TkImgBuf make_gradient_rgba(uint32_t w, uint32_t h)
{
    uint64_t len = (uint64_t)w * h * 4;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) {
        TkImgBuf empty;
        memset(&empty, 0, sizeof(empty));
        return empty;
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *p = data + ((uint64_t)y * w + x) * 4;
            p[0] = (w > 1) ? (uint8_t)(x * 255 / (w - 1)) : 0;
            p[1] = (h > 1) ? (uint8_t)(y * 255 / (h - 1)) : 0;
            p[2] = 128;
            p[3] = 255;
        }
    }
    TkImgBuf buf = image_from_raw(data, w, h, 4);
    free(data);
    return buf;
}

/* =========================================================================
 * Test 1: Full pipeline — create 64x64 -> resize 32x32 -> grayscale
 *         -> encode PNG -> decode -> verify
 * ========================================================================= */
static void test_full_pipeline(void)
{
    /* Step 1: Create 64x64 gradient RGBA */
    TkImgBuf orig = make_gradient_rgba(64, 64);
    ASSERT(orig.data != NULL,    "pipeline: created 64x64 RGBA");
    ASSERT(orig.width == 64,     "pipeline: width==64");
    ASSERT(orig.height == 64,    "pipeline: height==64");
    ASSERT(orig.channels == 4,   "pipeline: channels==4");

    /* Step 2: Resize to 32x32 */
    TkImgBuf resized = image_resize(orig, 32, 32);
    ASSERT(resized.data != NULL,    "pipeline: resize data non-null");
    ASSERT(resized.width == 32,     "pipeline: resized width==32");
    ASSERT(resized.height == 32,    "pipeline: resized height==32");
    ASSERT(resized.channels == 4,   "pipeline: resized channels==4");

    /* Step 3: Convert to grayscale */
    TkImgBuf gray = image_to_grayscale(resized);
    ASSERT(gray.data != NULL,    "pipeline: grayscale data non-null");
    ASSERT(gray.width == 32,     "pipeline: grayscale width==32");
    ASSERT(gray.height == 32,    "pipeline: grayscale height==32");
    ASSERT(gray.channels == 1,   "pipeline: grayscale channels==1");

    /* Step 4: Encode as PNG */
    ImgEncResult enc = image_encode(gray, IMG_FMT_PNG, 0);
    ASSERT(!enc.is_err,          "pipeline: PNG encode succeeded");
    ASSERT(enc.ok != NULL,       "pipeline: PNG bytes non-null");
    ASSERT(enc.ok_len > 0,       "pipeline: PNG bytes len > 0");

    /* Step 5: Decode PNG back */
    ImgResult dec = image_decode(enc.ok, enc.ok_len);
    ASSERT(!dec.is_err,          "pipeline: PNG decode succeeded");
    ASSERT(dec.ok.data != NULL,  "pipeline: decoded data non-null");
    ASSERT(dec.ok.width == 32,   "pipeline: decoded width==32");
    ASSERT(dec.ok.height == 32,  "pipeline: decoded height==32");
    ASSERT(dec.ok.channels == 1, "pipeline: decoded channels==1");

    /* Step 6: Verify pixel values are reasonable (not all zero/corrupt) */
    {
        int has_nonzero = 0;
        for (uint64_t i = 0; i < dec.ok.data_len; i++) {
            if (dec.ok.data[i] != 0) { has_nonzero = 1; break; }
        }
        ASSERT(has_nonzero, "pipeline: decoded pixels contain non-zero values");
    }

    /* Verify pixels match the grayscale image before encode */
    {
        int pixels_match = 1;
        for (uint64_t i = 0; i < gray.data_len && i < dec.ok.data_len; i++) {
            if (gray.data[i] != dec.ok.data[i]) {
                pixels_match = 0;
                break;
            }
        }
        ASSERT(pixels_match, "pipeline: decoded pixels match pre-encode grayscale");
    }

    image_buf_free(&orig);
    image_buf_free(&resized);
    image_buf_free(&gray);
    image_buf_free(&dec.ok);
    free(enc.ok);
}

/* =========================================================================
 * Test 2: Round-trip — create RGBA -> encode PNG -> decode -> verify pixels
 * ========================================================================= */
static void test_roundtrip_rgba(void)
{
    TkImgBuf orig = make_gradient_rgba(16, 16);
    ASSERT(orig.data != NULL, "roundtrip: created 16x16 RGBA");

    /* Encode */
    ImgEncResult enc = image_encode(orig, IMG_FMT_PNG, 0);
    ASSERT(!enc.is_err, "roundtrip: encode succeeded");

    /* Decode */
    ImgResult dec = image_decode(enc.ok, enc.ok_len);
    ASSERT(!dec.is_err,          "roundtrip: decode succeeded");
    ASSERT(dec.ok.width == 16,   "roundtrip: decoded width==16");
    ASSERT(dec.ok.height == 16,  "roundtrip: decoded height==16");
    ASSERT(dec.ok.channels == 4, "roundtrip: decoded channels==4");

    /* Verify pixel data matches original exactly */
    ASSERT(dec.ok.data_len == orig.data_len,
           "roundtrip: data_len matches");
    {
        int match = (dec.ok.data_len == orig.data_len) &&
                    (memcmp(orig.data, dec.ok.data, orig.data_len) == 0);
        ASSERT(match, "roundtrip: pixel data matches original exactly");
    }

    /* Spot-check a specific pixel: bottom-right (15,15) should have R=255, G=255 */
    {
        PixelResult px = image_pixel_at(dec.ok, 15, 15);
        ASSERT(!px.is_err, "roundtrip: pixel_at(15,15) ok");
        if (!px.is_err && px.ok) {
            ASSERT(px.ok[0] == 255, "roundtrip: px(15,15).R==255");
            ASSERT(px.ok[1] == 255, "roundtrip: px(15,15).G==255");
            ASSERT(px.ok[2] == 128, "roundtrip: px(15,15).B==128");
            ASSERT(px.ok[3] == 255, "roundtrip: px(15,15).A==255");
            free(px.ok);
        }
    }

    image_buf_free(&orig);
    image_buf_free(&dec.ok);
    free(enc.ok);
}

/* =========================================================================
 * Test 3: Edge case — 1x1 image through full pipeline
 * ========================================================================= */
static void test_1x1_pipeline(void)
{
    /* Create 1x1 RGBA */
    uint8_t pixel[4] = { 42, 99, 200, 255 };
    TkImgBuf tiny = image_from_raw(pixel, 1, 1, 4);
    ASSERT(tiny.data != NULL,    "1x1: created");
    ASSERT(tiny.width == 1,      "1x1: width==1");
    ASSERT(tiny.height == 1,     "1x1: height==1");

    /* Resize 1x1 -> 1x1 (identity) */
    TkImgBuf resized = image_resize(tiny, 1, 1);
    ASSERT(resized.data != NULL, "1x1: resize data non-null");
    ASSERT(resized.width == 1,   "1x1: resized width==1");
    ASSERT(resized.height == 1,  "1x1: resized height==1");

    /* Grayscale */
    TkImgBuf gray = image_to_grayscale(resized);
    ASSERT(gray.data != NULL,    "1x1: grayscale data non-null");
    ASSERT(gray.channels == 1,   "1x1: grayscale channels==1");
    ASSERT(gray.data[0] != 0,   "1x1: grayscale value non-zero");

    /* Encode -> Decode round trip */
    ImgEncResult enc = image_encode(gray, IMG_FMT_PNG, 0);
    ASSERT(!enc.is_err,          "1x1: encode succeeded");

    ImgResult dec = image_decode(enc.ok, enc.ok_len);
    ASSERT(!dec.is_err,          "1x1: decode succeeded");
    ASSERT(dec.ok.width == 1,    "1x1: decoded width==1");
    ASSERT(dec.ok.height == 1,   "1x1: decoded height==1");
    ASSERT(dec.ok.channels == 1, "1x1: decoded channels==1");
    ASSERT(dec.ok.data[0] == gray.data[0],
           "1x1: decoded pixel matches grayscale value");

    image_buf_free(&tiny);
    image_buf_free(&resized);
    image_buf_free(&gray);
    image_buf_free(&dec.ok);
    free(enc.ok);
}

/* =========================================================================
 * Test 4: Edge case — large image dimensions (256x256)
 * ========================================================================= */
static void test_large_image(void)
{
    TkImgBuf big = make_gradient_rgba(256, 256);
    ASSERT(big.data != NULL,      "large: created 256x256 RGBA");
    ASSERT(big.data_len == (uint64_t)256 * 256 * 4,
           "large: data_len == 256*256*4");

    /* Resize down to 64x64 */
    TkImgBuf resized = image_resize(big, 64, 64);
    ASSERT(resized.data != NULL,  "large: resized to 64x64");
    ASSERT(resized.width == 64,   "large: resized width==64");
    ASSERT(resized.height == 64,  "large: resized height==64");

    /* Grayscale */
    TkImgBuf gray = image_to_grayscale(resized);
    ASSERT(gray.data != NULL,     "large: grayscale non-null");
    ASSERT(gray.channels == 1,    "large: grayscale channels==1");

    /* Encode -> Decode */
    ImgEncResult enc = image_encode(gray, IMG_FMT_PNG, 0);
    ASSERT(!enc.is_err,           "large: encode succeeded");
    ASSERT(enc.ok_len > 8,        "large: PNG has valid size");

    ImgResult dec = image_decode(enc.ok, enc.ok_len);
    ASSERT(!dec.is_err,           "large: decode succeeded");
    ASSERT(dec.ok.width == 64,    "large: decoded width==64");
    ASSERT(dec.ok.height == 64,   "large: decoded height==64");

    /* Verify gradient is preserved: corner pixels should differ */
    {
        /* Top-left should be darker, bottom-right should be brighter */
        PixelResult tl = image_pixel_at(dec.ok, 0, 0);
        PixelResult br = image_pixel_at(dec.ok, 63, 63);
        ASSERT(!tl.is_err && !br.is_err,
               "large: corner pixel reads succeeded");
        if (!tl.is_err && !br.is_err && tl.ok && br.ok) {
            ASSERT(br.ok[0] > tl.ok[0],
                   "large: bottom-right brighter than top-left");
            free(tl.ok);
            free(br.ok);
        }
    }

    image_buf_free(&big);
    image_buf_free(&resized);
    image_buf_free(&gray);
    image_buf_free(&dec.ok);
    free(enc.ok);
}

/* =========================================================================
 * Test 5: Grayscale preserves dimension through encode/decode
 * ========================================================================= */
static void test_grayscale_roundtrip(void)
{
    /* Create RGBA, convert to grayscale, encode, decode, re-encode, decode
     * again — dimensions and pixel data should remain stable. */
    TkImgBuf orig = make_gradient_rgba(8, 8);
    TkImgBuf gray = image_to_grayscale(orig);
    ASSERT(gray.data != NULL, "gray-rt: grayscale non-null");

    /* First round trip */
    ImgEncResult enc1 = image_encode(gray, IMG_FMT_PNG, 0);
    ASSERT(!enc1.is_err, "gray-rt: first encode ok");

    ImgResult dec1 = image_decode(enc1.ok, enc1.ok_len);
    ASSERT(!dec1.is_err, "gray-rt: first decode ok");

    /* Second round trip */
    ImgEncResult enc2 = image_encode(dec1.ok, IMG_FMT_PNG, 0);
    ASSERT(!enc2.is_err, "gray-rt: second encode ok");

    ImgResult dec2 = image_decode(enc2.ok, enc2.ok_len);
    ASSERT(!dec2.is_err, "gray-rt: second decode ok");

    /* Dimensions must match */
    ASSERT(dec2.ok.width == 8,    "gray-rt: final width==8");
    ASSERT(dec2.ok.height == 8,   "gray-rt: final height==8");
    ASSERT(dec2.ok.channels == 1, "gray-rt: final channels==1");

    /* Pixel data must be identical after two round trips */
    {
        int match = (dec2.ok.data_len == gray.data_len) &&
                    (memcmp(gray.data, dec2.ok.data, gray.data_len) == 0);
        ASSERT(match, "gray-rt: pixels stable after two round trips");
    }

    image_buf_free(&orig);
    image_buf_free(&gray);
    image_buf_free(&dec1.ok);
    image_buf_free(&dec2.ok);
    free(enc1.ok);
    free(enc2.ok);
}

/* =========================================================================
 * Test 6: Resize up from 1x1 to 4x4 — all pixels should be same colour
 * ========================================================================= */
static void test_resize_upscale(void)
{
    uint8_t pixel[4] = { 100, 150, 200, 255 };
    TkImgBuf tiny = image_from_raw(pixel, 1, 1, 4);
    ASSERT(tiny.data != NULL, "upscale: 1x1 created");

    TkImgBuf big = image_resize(tiny, 4, 4);
    ASSERT(big.data != NULL,  "upscale: resized to 4x4");
    ASSERT(big.width == 4,    "upscale: width==4");
    ASSERT(big.height == 4,   "upscale: height==4");

    /* All 16 pixels should be the same colour (bilinear of uniform = uniform) */
    {
        int uniform = 1;
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t *p = big.data + i * 4;
            if (p[0] != 100 || p[1] != 150 || p[2] != 200 || p[3] != 255) {
                uniform = 0;
                break;
            }
        }
        ASSERT(uniform, "upscale: all pixels match source colour");
    }

    image_buf_free(&tiny);
    image_buf_free(&big);
}

/* =========================================================================
 * main
 * ========================================================================= */
int main(void)
{
    printf("=== image pipeline integration tests ===\n");

    test_full_pipeline();
    test_roundtrip_rgba();
    test_1x1_pipeline();
    test_large_image();
    test_grayscale_roundtrip();
    test_resize_upscale();

    printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
