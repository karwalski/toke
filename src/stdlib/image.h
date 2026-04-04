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

#endif /* TK_STDLIB_IMAGE_H */
