#ifndef TK_STDLIB_IMAGE_H
#define TK_STDLIB_IMAGE_H

/*
 * image.h — C interface for the std.image standard library module.
 *
 * Type mappings:
 *   imgbuf  = TkImgBuf  (width, height, channels, heap-allocated pixel data)
 *   imgfmt  = TkImgFmt  (PNG, JPEG, WebP, BMP)
 *   u32     = uint32_t
 *   u8      = uint8_t
 *   [byte]  = uint8_t * + length
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 * PNG encode/decode use a pure-C99 DEFLATE implementation (RFC 1951).
 * JPEG, WebP, BMP encode/decode are stubbed and return an error directing
 * the caller to link the appropriate library.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own all returned heap pointers; use image_buf_free()
 * to release a TkImgBuf and free() for raw byte pointers.
 *
 * Story: 18.1.6
 */

#include <stdint.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
 * Core image buffer type
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t  width;
    uint32_t  height;
    uint8_t   channels;  /* 1=gray, 3=RGB, 4=RGBA */
    uint8_t  *data;      /* heap-allocated pixel data, row-major */
    uint64_t  data_len;
} TkImgBuf;

/* -----------------------------------------------------------------------
 * Supported image formats
 * ----------------------------------------------------------------------- */

typedef enum {
    IMG_FMT_PNG,
    IMG_FMT_JPEG,
    IMG_FMT_WEBP,
    IMG_FMT_BMP
} TkImgFmt;

/* -----------------------------------------------------------------------
 * Result types
 * ----------------------------------------------------------------------- */

/* Decode result: either a valid TkImgBuf or an error string. */
typedef struct {
    TkImgBuf    ok;
    int         is_err;
    const char *err_msg;
} ImgResult;

/* Encode result: either a byte buffer or an error string. */
typedef struct {
    uint8_t    *ok;
    uint64_t    ok_len;
    int         is_err;
    const char *err_msg;
} ImgEncResult;

/* Pixel result: either a byte buffer (channels bytes) or an error string. */
typedef struct {
    uint8_t    *ok;
    uint64_t    ok_len;
    int         is_err;
    const char *err_msg;
} PixelResult;

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

/* image.decode([byte]) imgbuf!str
 * Decode PNG bytes into a TkImgBuf.
 * Other formats return is_err=1 with an instructional err_msg. */
ImgResult    image_decode(const uint8_t *bytes, uint64_t len);

/* image.encode(imgbuf, imgfmt, u8) [byte]!str
 * Encode a TkImgBuf to the given format.
 * quality is used for lossy formats (0-100); ignored for PNG/BMP.
 * Non-PNG formats return is_err=1 with an instructional err_msg. */
ImgEncResult image_encode(TkImgBuf buf, TkImgFmt fmt, uint8_t quality);

/* image.resize(imgbuf, u32, u32) imgbuf
 * Return a new TkImgBuf scaled to (width x height) using bilinear
 * interpolation.  Returns a zero TkImgBuf (data==NULL) on OOM. */
TkImgBuf     image_resize(TkImgBuf buf, uint32_t width, uint32_t height);

/* image.crop(imgbuf, u32, u32, u32, u32) imgbuf!str
 * Crop the image to the rectangle (x, y, width, height).
 * Returns is_err=1 if the rectangle exceeds the image bounds. */
ImgResult    image_crop(TkImgBuf buf, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height);

/* image.to_grayscale(imgbuf) imgbuf
 * Convert any image to a single-channel (channels=1) grayscale image using
 * ITU-R BT.601 luminance coefficients. */
TkImgBuf     image_to_grayscale(TkImgBuf buf);

/* image.flip_h(imgbuf) imgbuf
 * Return a copy of the image with pixels in each row reversed. */
TkImgBuf     image_flip_h(TkImgBuf buf);

/* image.flip_v(imgbuf) imgbuf
 * Return a copy of the image with the row order reversed. */
TkImgBuf     image_flip_v(TkImgBuf buf);

/* image.pixel_at(imgbuf, u32, u32) [byte]!str
 * Return a heap-allocated buffer of `channels` bytes for pixel (x, y).
 * Returns is_err=1 if (x, y) is outside the image bounds. */
PixelResult  image_pixel_at(TkImgBuf buf, uint32_t x, uint32_t y);

/* image.from_raw([byte], u32, u32, u8) imgbuf
 * Create a TkImgBuf by copying `data` (width*height*channels bytes).
 * Returns a zero TkImgBuf (data==NULL) on OOM. */
TkImgBuf     image_from_raw(const uint8_t *data, uint32_t width,
                             uint32_t height, uint8_t channels);

/* Free the data pointer inside a TkImgBuf (sets pointer to NULL). */
void         image_buf_free(TkImgBuf *buf);

/* -----------------------------------------------------------------------
 * Transforms and filters (Story 34.3.1)
 * ----------------------------------------------------------------------- */

/* image.rotate(imgbuf, f64) imgbuf
 * Return a new image (same size) rotated by angle_deg degrees around the
 * centre using inverse-mapping bilinear interpolation.  Out-of-bounds
 * source samples are filled with 0. */
TkImgBuf     image_rotate(TkImgBuf buf, double angle_deg);

/* image.blur(imgbuf, int) imgbuf
 * Box-approximate Gaussian blur.  The 3×3 kernel [1,2,1;2,4,2;1,2,1]/16 is
 * applied `radius` times.  Returns a zero TkImgBuf on OOM. */
TkImgBuf     image_blur(TkImgBuf buf, int radius);

/* image.sharpen(imgbuf) imgbuf
 * Apply a 3×3 sharpening kernel (centre weight 5, cardinal neighbours -1).
 * Each channel is clamped to [0,255]. */
TkImgBuf     image_sharpen(TkImgBuf buf);

/* image.brightness(imgbuf, f64) imgbuf
 * Multiply every non-alpha channel by factor and clamp to [0,255].
 * factor > 1.0 brightens, < 1.0 darkens. */
TkImgBuf     image_brightness(TkImgBuf buf, double factor);

/* image.contrast(imgbuf, f64) imgbuf
 * Apply contrast adjustment: new = 128 + (old - 128) * factor, clamped to
 * [0,255] per channel (not applied to the alpha channel). */
TkImgBuf     image_contrast(TkImgBuf buf, double factor);

/* image.paste(dst, src, int, int) imgbuf
 * Composite src onto a copy of dst with src's top-left at (x, y).
 * Src pixels that fall outside dst are silently clipped.
 * If channels == 4, alpha blending is applied: out = a*src + (1-a)*dst. */
TkImgBuf     image_paste(TkImgBuf dst, TkImgBuf src, int x, int y);

/* -----------------------------------------------------------------------
 * Histogram, quantization, and text drawing (Story 34.3.2)
 * ----------------------------------------------------------------------- */

/* Pixel histogram: 256-bin per channel. */
typedef struct {
    uint64_t r[256];
    uint64_t g[256];
    uint64_t b[256];
    uint64_t a[256];
} ImgHistogram;

/* image.histogram(imgbuf) ImgHistogram
 * Walk all pixels and count per-channel values [0..255].
 * For RGB images (channels==3), a[] is left zeroed.
 * For grayscale images (channels==1), only r[] is filled. */
ImgHistogram image_histogram(TkImgBuf buf);

/* image.quantize(imgbuf, u64) imgbuf
 * Reduce the image palette to at most ncolors distinct colors using the
 * median-cut algorithm.  Returns a new TkImgBuf (same dimensions, same
 * channel count) with quantized colors.  Returns a zero TkImgBuf (data==NULL)
 * on OOM or invalid input. */
TkImgBuf     image_quantize(TkImgBuf buf, uint64_t ncolors);

/* image.text_draw(imgbuf, str, int, int, int, u32) imgbuf
 * Draw text onto a copy of buf using a built-in 5×7 bitmap font.
 * x, y    — top-left origin of the first character (may be negative)
 * size    — pixel scale factor (1 = native 5×7, 2 = 10×14, …)
 * color   — 0xRRGGBBAA for RGBA images, 0xRRGGBB__ for RGB images
 * Only ASCII printable characters (32–126) are rendered; others are skipped.
 * Returns a new TkImgBuf (caller must free); returns zero buf on OOM. */
TkImgBuf     image_text_draw(TkImgBuf buf, const char *text,
                              int x, int y, int size, uint32_t color);

#endif /* TK_STDLIB_IMAGE_H */
