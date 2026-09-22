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
    tk_current_error = r.is_err ? 1 : 0;
    if (r.is_err) return 0;
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
    tk_current_error = r.is_err ? 1 : 0;
    if (r.is_err) return tk_arr_alloc(0, 0);
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
    tk_current_error = r.is_err ? 1 : 0;
    if (r.is_err) return 0;
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
    tk_current_error = r.is_err ? 1 : 0;
    if (r.is_err) return tk_arr_alloc(0, 0);
    int64_t out = tk_bytes_pack(r.ok, r.ok_len);
    free(r.ok);
    return out;
}

/* image.fromraw(bytes; width; height; channels) : $imgbuf */
int64_t tk_image_fromraw_w(int64_t bytes, int64_t width, int64_t height,
                           int64_t channels) {
    uint8_t *buf = NULL;
    tk_bytes_unpack(bytes, &buf);
    TkImgBuf out = image_from_raw(buf, (uint32_t)width, (uint32_t)height,
                                  (uint8_t)channels);
    free(buf);
    int64_t h = imgbuf_to_toke(out);
    image_buf_free(&out);
    return h;
}
