/*
 * fuzz_image.c — libFuzzer entry point for std.image's decoders (122.9).
 *
 * image.c parses untrusted bytes with its own PNG chunk walker, its own
 * DEFLATE/Huffman inflater and a TIFF IFD/strip decoder (PackBits, LZW,
 * Deflate).  This target drives all of them:
 *   - image_decode()       PNG (CRC gate, IHDR size cap, capped inflate,
 *                          unfilter) and, by signature, TIFF page 0
 *   - image_tiff_pages()   the IFD chain walk
 *   - image_tiff_decode()  page 1, which reaches the chain's second link
 * and, when a decode succeeds, the transforms a thumbnailer would run on the
 * result (encode, resize, flip, crop, pixelat), so a decoder that returned
 * a buffer inconsistent with its own dimensions is caught where it is used.
 *
 * Standalone mode (no libFuzzer runtime, e.g. Apple clang): link against
 * test/fuzz/standalone_main.c and it replays files; see
 * test/conform/C138_image_decoder_hardening.sh, which does exactly that
 * over test/fuzz/seeds/image/ under ASan+UBSan.
 *
 * Story: 122.9
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#include "../../src/stdlib/image.h"

static void exercise(TkImgBuf b)
{
    ImgEncResult e = image_encode(b, IMG_FMT_PNG, 0);
    if (!e.is_err) {
        /* round trip: our own encoder's output must decode */
        ImgResult r2 = image_decode(e.ok, e.ok_len);
        if (!r2.is_err) image_buf_free(&r2.ok);
        free(e.ok);
    }
    TkImgBuf s = image_resize(b, 3, 2);
    image_buf_free(&s);
    TkImgBuf f = image_flip_h(b);
    image_buf_free(&f);
    ImgResult c = image_crop(b, 0, 0, b.width > 1 ? 1 : b.width,
                             b.height > 1 ? 1 : b.height);
    if (!c.is_err) image_buf_free(&c.ok);
    PixelResult p = image_pixel_at(b, b.width - 1, b.height - 1);
    if (!p.is_err) free(p.ok);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > (1u << 20)) return 0;

    ImgResult r = image_decode(data, size);
    if (!r.is_err) {
        exercise(r.ok);
        image_buf_free(&r.ok);
    }

    (void)image_tiff_pages(data, size);
    ImgResult t = image_tiff_decode(data, size, 1);
    if (!t.is_err) image_buf_free(&t.ok);
    return 0;
}
