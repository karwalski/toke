/*
 * image_glue.c — i64-ABI wrappers for std.image (Story 136.32).
 *
 * std.image is one of the six modules src/stdlib_deps.c registered with no
 * glue file. image.c has carried a self-contained PNG codec and the whole
 * transform set since story 18.1.6 and stdlib/image.tki exports nine entry
 * points; nothing wrapped them, so the thumbnail example on
 * docs/stdlib/image.md failed at link on image.decode and image.resize.
 *
 * `$imgbuf{width:u32; height:u32; channels:u8; data:[byte]}` crosses as a
 * pointer to a four-slot block in that field order, with `data` a toke
 * [byte] in the bytes_rt.h representation. The C TkImgBuf additionally
 * carries data_len, which is recoverable from the array's own length, so
 * nothing is lost by round-tripping through the documented struct rather
 * than hiding a TkImgBuf pointer behind it.
 */
#include "image.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_array.h"
#include "bytes_rt.h"

/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

/* 135.5b — a `T!str` error carries its message.  runtime-abi.md §7.3: for an
 * error type of plain `str` the slot holds the message pointer itself (never
 * 0 or 1 — a real pointer), and the $err arm binds it as the str.  Before
 * this every failure stored the payload-less flag 1, so "not a TIFF" and
 * "unsupported compression" were indistinguishable to a toke caller. */
static void img_raise(const char *msg) {
    tk_current_error = (int64_t)(intptr_t)(msg && msg[0] ? msg : "image: operation failed");
}

/* Slot offsets from stdlib/image.tki's $imgbuf field order. */
#define IMG_W 0
#define IMG_H 1
#define IMG_C 2
#define IMG_D 3

static int64_t imgbuf_to_toke(TkImgBuf b) {
    int64_t *s = (int64_t *)malloc(4 * sizeof(int64_t));
    if (!s) return 0;
    s[IMG_W] = (int64_t)b.width;
    s[IMG_H] = (int64_t)b.height;
    s[IMG_C] = (int64_t)b.channels;
    s[IMG_D] = tk_bytes_pack(b.data ? b.data : (const uint8_t *)"", b.data_len);
    return (int64_t)(intptr_t)s;
}

/* Rebuild a TkImgBuf from a toke $imgbuf. The pixel buffer is freshly
 * malloc'd (the C transforms read it and image_buf_free() would otherwise be
 * freeing memory the toke array still owns); *owned receives it to free. */
static TkImgBuf imgbuf_from_toke(int64_t h, uint8_t **owned) {
    TkImgBuf b; memset(&b, 0, sizeof b);
    *owned = NULL;
    if (!h) return b;
    int64_t *s = (int64_t *)(intptr_t)h;
    b.width    = (uint32_t)s[IMG_W];
    b.height   = (uint32_t)s[IMG_H];
    b.channels = (uint8_t)s[IMG_C];
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(s[IMG_D], &buf);
    b.data     = buf;
    b.data_len = n;
    *owned = buf;
    return b;
}

/* image.decode(bytes) : $imgbuf!$str */
int64_t tk_image_decode_w(int64_t bytes) {
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(bytes, &buf);
    ImgResult r = image_decode(buf, n);
    free(buf);
    if (r.is_err) { img_raise(r.err_msg); return 0; }
    tk_current_error = 0;
    int64_t out = imgbuf_to_toke(r.ok);
    image_buf_free(&r.ok);
    return out;
}

/* image.encode(buf; fmt; quality) : @(byte)!$str */
int64_t tk_image_encode_w(int64_t buf, int64_t fmt, int64_t quality) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);
    ImgEncResult r = image_encode(b, (TkImgFmt)fmt, (uint8_t)quality);
    free(owned);
    if (r.is_err) { img_raise(r.err_msg); return tk_arr_alloc(0, 0); }
    tk_current_error = 0;
    int64_t out = tk_bytes_pack(r.ok, r.ok_len);
    free(r.ok);
    return out;
}

/* image.resize(buf; width; height) : $imgbuf */
int64_t tk_image_resize_w(int64_t buf, int64_t width, int64_t height) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);
    TkImgBuf out = image_resize(b, (uint32_t)width, (uint32_t)height);
    free(owned);
    int64_t h = imgbuf_to_toke(out);
    image_buf_free(&out);
    return h;
}

/* image.tograyscale / fliph / flipv — same one-buffer-in, one-buffer-out
 * shape, all exported by stdlib/image.tki and all previously unreachable. */
#define TK_IMG_XFORM(name, fn)                                   \
    int64_t name(int64_t buf) {                                  \
        uint8_t *owned = NULL;                                   \
        TkImgBuf b = imgbuf_from_toke(buf, &owned);              \
        TkImgBuf out = fn(b);                                    \
        free(owned);                                             \
        int64_t h = imgbuf_to_toke(out);                         \
        image_buf_free(&out);                                    \
        return h;                                                \
    }

TK_IMG_XFORM(tk_image_tograyscale_w, image_to_grayscale)
TK_IMG_XFORM(tk_image_fliph_w,       image_flip_h)
TK_IMG_XFORM(tk_image_flipv_w,       image_flip_v)

/* image.crop(buf; x; y; w; h) : $imgbuf!$str */
int64_t tk_image_crop_w(int64_t buf, int64_t x, int64_t y, int64_t w, int64_t h) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);
    ImgResult r = image_crop(b, (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    free(owned);
    if (r.is_err) { img_raise(r.err_msg); return 0; }
    tk_current_error = 0;
    int64_t out = imgbuf_to_toke(r.ok);
    image_buf_free(&r.ok);
    return out;
}

/* image.pixelat(buf; x; y) : @(byte)!$str */
int64_t tk_image_pixelat_w(int64_t buf, int64_t x, int64_t y) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);
    PixelResult r = image_pixel_at(b, (uint32_t)x, (uint32_t)y);
    free(owned);
    if (r.is_err) { img_raise(r.err_msg); return tk_arr_alloc(0, 0); }
    tk_current_error = 0;
    int64_t out = tk_bytes_pack(r.ok, r.ok_len);
    free(r.ok);
    return out;
}

/* image.fromraw(bytes; width; height; channels) : $imgbuf */
int64_t tk_image_fromraw_w(int64_t bytes, int64_t width, int64_t height,
                           int64_t channels) {
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(bytes, &buf);
    /* 122.9: image_from_raw copies width*height*channels bytes and cannot
     * see how many `bytes` holds; the length the array carries was dropped
     * here, so a short array was a heap over-read.  Out-of-range arguments
     * are refused (an empty $imgbuf) rather than truncated into u32/u8, and
     * a short array is zero-padded to the declared size. */
    if (width <= 0 || height <= 0 || channels <= 0 || channels > 4 ||
        width > 0xFFFFFFFFll || height > 0xFFFFFFFFll) {
        free(buf);
        TkImgBuf none; memset(&none, 0, sizeof none);
        return imgbuf_to_toke(none);
    }
    uint64_t px = (uint64_t)width * (uint64_t)height;
    uint64_t need = px <= (UINT64_MAX / 4) ? px * (uint64_t)channels : UINT64_MAX;
    if (n < need && need <= (1ull << 30)) {
        uint8_t *grown = (uint8_t *)realloc(buf, (size_t)need);
        if (!grown) { free(buf); buf = NULL; }
        else { memset(grown + n, 0, (size_t)(need - n)); buf = grown; }
    } else if (n < need) {
        free(buf); buf = NULL;     /* over the cap: image_from_raw refuses it */
    }
    TkImgBuf out = image_from_raw(buf, (uint32_t)width, (uint32_t)height,
                                  (uint8_t)channels);
    free(buf);
    int64_t h = imgbuf_to_toke(out);
    image_buf_free(&out);
    return h;
}

/* =========================================================================
 * Story 135.5 — the four document-processing gaps.
 *
 * Two of the four needed no new C.  image_rotate (inverse-mapped bilinear)
 * and image_blur have been in image.c since 34.3.1; they had no wrapper here
 * and no entry in stdlib/image.tki, so no toke program could reach them.
 * The other two — image_adaptive_threshold and image_convolve — are new, as
 * is the TIFF decoder behind tiffpages / tiffdecode.
 *
 * ON ERROR MESSAGES.  A failure stores the C err_msg in the slot (img_raise,
 * 135.5b), and the $err arm of a `!str` call binds it, so image.c's messages
 * reach toke.  Until 135.5b every failure stored the payload-less flag 1 and
 * the arm bound nil.
 * ========================================================================= */

static double img_i64_to_f64(int64_t i) { double d; memcpy(&d, &i, sizeof(d)); return d; }

/* ImgResult -> ($imgbuf, tk_current_error) for the `imgbuf!str` returns. */
static int64_t imgres_to_toke(ImgResult r) {
    if (r.is_err) { img_raise(r.err_msg); return 0; }
    tk_current_error = 0;
    int64_t out = imgbuf_to_toke(r.ok);
    image_buf_free(&r.ok);
    return out;
}

/* image.rotate(buf; angledeg) : $imgbuf
 * Arbitrary angle, bilinear interpolation, output the same size as the
 * input — which is what deskew needs and what fliph/flipv cannot express. */
int64_t tk_image_rotate_w(int64_t buf, int64_t angle) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);
    TkImgBuf out = image_rotate(b, img_i64_to_f64(angle));
    free(owned);
    int64_t h = imgbuf_to_toke(out);
    image_buf_free(&out);
    return h;
}

/* image.blur(buf; passes) : $imgbuf */
int64_t tk_image_blur_w(int64_t buf, int64_t passes) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);
    TkImgBuf out = image_blur(b, (int)passes);
    free(owned);
    int64_t h = imgbuf_to_toke(out);
    image_buf_free(&out);
    return h;
}

/* image.adaptivethreshold(buf; window; k) : $imgbuf!$str */
int64_t tk_image_adaptivethreshold_w(int64_t buf, int64_t window, int64_t k) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);
    ImgResult r = image_adaptive_threshold(b, (uint32_t)window,
                                           img_i64_to_f64(k));
    free(owned);
    return imgres_to_toke(r);
}

/* image.convolve(buf; kernel; ksize; divisor; offset) : $imgbuf!$str
 * `kernel` is a toke [f64]: its backing block holds the doubles bitcast into
 * i64 slots, contiguously, so the handle is already a double* (math_glue.c
 * decode_f64_array). */
int64_t tk_image_convolve_w(int64_t buf, int64_t kern, int64_t ksize,
                            int64_t divisor, int64_t offset) {
    uint8_t *owned = NULL;
    TkImgBuf b = imgbuf_from_toke(buf, &owned);

    int64_t klen = tk_arr_len(kern);
    /* The declared size and the array must agree, or the kernel read would
     * run off the end of the block. */
    if (!kern || klen <= 0 || ksize <= 0 ||
        klen != (int64_t)ksize * (int64_t)ksize) {
        free(owned);
        img_raise("image.convolve: kernel length must be ksize*ksize and non-empty");
        return 0;
    }

    ImgResult r = image_convolve(b, (const double *)(intptr_t)kern,
                                 (uint32_t)ksize,
                                 img_i64_to_f64(divisor),
                                 img_i64_to_f64(offset));
    free(owned);
    return imgres_to_toke(r);
}

/* image.tiffpages(bytes) : u32!$str — the number of pages (IFDs). */
int64_t tk_image_tiffpages_w(int64_t bytes) {
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(bytes, &buf);
    int64_t pages = image_tiff_pages(buf, n);
    free(buf);
    if (pages < 0) {
        img_raise("image.tiffpages: not a TIFF file, or its IFD chain is malformed");
        return 0;
    }
    tk_current_error = 0;
    return pages;
}

/* image.tiffdecode(bytes; page) : $imgbuf!$str — one 0-based page. */
int64_t tk_image_tiffdecode_w(int64_t bytes, int64_t page) {
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(bytes, &buf);
    ImgResult r = image_tiff_decode(buf, n,
                                    page < 0 ? 0xFFFFFFFFu : (uint32_t)page);
    free(buf);
    return imgres_to_toke(r);
}
