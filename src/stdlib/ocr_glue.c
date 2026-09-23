/*
 * ocr_glue.c — i64-ABI wrappers for the std.ocr module (story 135.6).
 *
 * Registered against module "ocr" in src/stdlib_deps.c.  136.33 is why that
 * sentence is here: a glue file registered under the wrong module name
 * leaves the module unlinkable ON ITS OWN, and nothing shows it up until a
 * program imports that module and nothing else.  test/conform/C033 compiles
 * such a program as its first case.
 *
 * ABI notes (docs/runtime-abi.md):
 *   §3  a `str` is a plain NUL-terminated char*, cast through i64.
 *   §4  an array is a length-prefixed i64 block; the handle points at
 *       data[0] and the length sits at handle[-1] (tk_array.h).  A `@(byte)`
 *       stores ONE BYTE PER i64 SLOT (bytes_rt.h) — hence tk_bytes_unpack.
 *   §5  a struct is a flat i64 block, one slot per declared field, in
 *       declaration order.
 *
 * §5 is the one that bites, exactly as it bit std.pdf: FIVE of the seven
 * slots are f64, and an f64 field travels as the BIT PATTERN of the double
 * in an i64 slot.  A slot written with `(int64_t)value` instead of
 * `f64_to_i64(value)` yields the integer 1 where the page says 1.0, or 0
 * where it says 0.5.  That compiles, runs, and prints a plausible number.
 *
 * A `$ocrrun` built here MUST have exactly the seven slots stdlib/ocr.tki
 * declares, in that order — which are std.pdf's `$textrun` slots, with the
 * SAME names and types, for the first six:
 *
 *     slot 0  text        str   NUL-terminated UTF-8
 *     slot 1  page        i64   plain integer, always 1 for one image
 *     slot 2  x           f64   bit pattern, pixels from the LEFT
 *     slot 3  y           f64   bit pattern, pixels from the BOTTOM
 *     slot 4  width       f64   bit pattern
 *     slot 5  height      f64   bit pattern
 *     slot 6  confidence  f64   bit pattern   <- where $textrun has fontsize
 *
 * The two records are therefore bit-identical in layout, which is the
 * point: the consumer this epic exists for routes text pages to pdf.runs
 * and scanned pages here and feeds BOTH into one field extractor.
 *
 * C033 reads every one of those fields and asserts its value, which is the
 * only defence against the shape silently drifting from the .tki.
 *
 * THE RETURN CONVENTION.  0 is the `$err` arm of a compiled error union.
 * An empty result is NOT 0: tk_arr_alloc(0, 0) returns a real handle, so a
 * page with no text at all comes back as a SUCCESS with zero runs and a
 * caller can tell it from "this platform has no recogniser", which is the
 * whole of requirement 5 on this story.  ocr.lasterrkind() carries which.
 */

#include "ocr.h"
#include "tk_array.h"
#include "bytes_rt.h"
#include "capabilities.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Number of i64 slots in an $ocrrun — keep in sync with stdlib/ocr.tki. */
#define TK_OCRRUN_SLOTS 7

static int64_t dup_str(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)malloc(n + 1);
    if (!p) return 0;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return (int64_t)(intptr_t)p;
}

/* An f64 crosses the i64 ABI as its bit pattern, never as a conversion. */
static int64_t f64_to_i64(double d)
{
    int64_t i;
    memcpy(&i, &d, sizeof i);
    return i;
}

/* One $ocrrun as a flat seven-slot block. */
static int64_t build_run(const TkOcrRun *r)
{
    int64_t *slot = (int64_t *)malloc(TK_OCRRUN_SLOTS * sizeof(int64_t));
    int64_t text;
    if (!slot) return 0;
    text = dup_str(r->text);
    if (!text) { free(slot); return 0; }
    slot[0] = text;                      /* .text       */
    slot[1] = r->page;                   /* .page       */
    slot[2] = f64_to_i64(r->x);          /* .x          */
    slot[3] = f64_to_i64(r->y);          /* .y          */
    slot[4] = f64_to_i64(r->width);      /* .width      */
    slot[5] = f64_to_i64(r->height);     /* .height     */
    slot[6] = f64_to_i64(r->confidence); /* .confidence */
    return (int64_t)(intptr_t)slot;
}

/* A whole TkOcrPage as @($ocrrun).  Frees the page; 0 only on OOM. */
static int64_t build_runs(TkOcrPage *page)
{
    uint32_t n, i, built = 0;
    int64_t arr;
    int64_t *block;

    n   = ocr_run_count(page);
    arr = tk_arr_alloc((int64_t)n, (int64_t)n);
    if (!arr) { ocr_page_free(page); return 0; }
    block = (int64_t *)(intptr_t)arr;

    for (i = 0; i < n; i++) {
        const TkOcrRun *r = ocr_run_at(page, i);
        int64_t v;
        if (!r) break;
        v = build_run(r);
        if (!v) break;
        block[built++] = v;
    }
    tk_arr_setlen(arr, (int64_t)built);
    ocr_page_free(page);
    return arr;
}

/* ocr.isavailable() -> bool
 *
 * A REAL PROBE.  ocr_is_available() resolves both Vision classes, confirms
 * every selector this module sends is implemented, and asks the engine for
 * its supported-language list — the check that fails when the framework is
 * present but its text model is not.
 *
 * std.mlx is the counter-example this deliberately does not follow:
 * tk_mlx_isavailable_w() hard-returns 0 in BOTH arms of its #if and never
 * calls the real mlx_is_available() probe sitting in the same module. */
int64_t tk_ocr_isavailable_w(void)
{
    return ocr_is_available() ? 1 : 0;
}

/* ocr.engine() -> str — "vision" or "none".
 *
 * The seam.  A caller routes on this and on ocr.isavailable() and never
 * names a platform, so story 135.8 can be added as a second engine behind
 * these same calls without touching a consumer. */
int64_t tk_ocr_engine_w(void)
{
    return (int64_t)(intptr_t)ocr_engine();
}

/* ocr.languages() -> str — comma-separated BCP-47, queried live. */
int64_t tk_ocr_languages_w(void)
{
    return (int64_t)(intptr_t)ocr_languages();
}

/* ocr.setlanguages(langs:str) -> bool
 *
 * false, with lasterrkind "nolanguage", if any tag is one the engine does
 * not have.  Vision silently ignores an unknown tag and recognises in its
 * default language, so accepting one would produce confident output in a
 * language the caller never asked for. */
int64_t tk_ocr_setlanguages_w(int64_t langs)
{
    return ocr_set_languages(langs ? (const char *)(intptr_t)langs : NULL) ? 1 : 0;
}

/* ocr.setfast(fast:bool) -> void — 0 accurate (default), 1 fast. */
int64_t tk_ocr_setfast_w(int64_t fast)
{
    ocr_set_fast(fast ? 1 : 0);
    return 0;
}

/* ocr.runs(data:@(byte)) -> @($ocrrun)!$ocrerr
 *
 * Encoded image bytes — PNG, JPEG, TIFF, BMP, GIF, HEIC, whatever ImageIO
 * on the host decodes. */
int64_t tk_ocr_runs_w(int64_t data)
{
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    TkOcrPage *page;

    if (!buf) return 0;
    page = ocr_recognize_mem(buf, n);
    free(buf);
    if (!page) return 0;
    return build_runs(page);
}

/* ocr.runsfile(path:str) -> @($ocrrun)!$ocrerr
 *
 * The route a large scan must take: a toke `@(byte)` costs eight bytes per
 * byte, so a 20 MB TIFF wants 160 MB of toke array before a pixel is
 * decoded.  Gated on fs.read like every other filesystem sink (124.4c). */
int64_t tk_ocr_runsfile_w(int64_t path)
{
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    {
        TkOcrPage *page = ocr_recognize_file((const char *)(intptr_t)path);
        if (!page) return 0;
        return build_runs(page);
    }
}

/* ocr.runsraw(px:@(byte); width:i64; height:i64; channels:i64)
 *     -> @($ocrrun)!$ocrerr
 *
 * The composition point with std.image: greyscale, crop, rotate and
 * adaptive-threshold there, then recognise here with no re-encode.  Rows
 * are tightly packed, TOP row first, which is std.image's layout;
 * `channels` is 1, 3 or 4. */
int64_t tk_ocr_runsraw_w(int64_t px, int64_t width, int64_t height,
                         int64_t channels)
{
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(px, &buf);
    TkOcrPage *page;

    if (!buf) return 0;
    /* Refuse a buffer that cannot hold the stated geometry rather than
     * reading off the end of it. */
    if (width <= 0 || height <= 0 || channels <= 0 ||
        (uint64_t)width * (uint64_t)height * (uint64_t)channels > n) {
        free(buf);
        return 0;
    }
    page = ocr_recognize_raw(buf, (uint32_t)width, (uint32_t)height,
                             (uint32_t)channels);
    free(buf);
    if (!page) return 0;
    return build_runs(page);
}

/* ocr.text(data:@(byte)) -> str!$ocrerr
 *
 * Runs joined with '\n' in the engine's own order.  An image with no text
 * yields "" — a real, non-zero pointer — and a platform with no recogniser
 * yields the `$err` arm.  Those are different answers on purpose. */
int64_t tk_ocr_text_w(int64_t data)
{
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    TkOcrPage *page;
    char *t;
    int64_t out;

    if (!buf) return 0;
    page = ocr_recognize_mem(buf, n);
    free(buf);
    if (!page) return 0;
    t = ocr_page_text(page);
    ocr_page_free(page);
    if (!t) return 0;
    out = dup_str(t);
    free(t);
    return out;
}

/* ocr.textfile(path:str) -> str!$ocrerr */
int64_t tk_ocr_textfile_w(int64_t path)
{
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    {
        TkOcrPage *page = ocr_recognize_file((const char *)(intptr_t)path);
        char *t;
        int64_t out;
        if (!page) return 0;
        t = ocr_page_text(page);
        ocr_page_free(page);
        if (!t) return 0;
        out = dup_str(t);
        free(t);
        return out;
    }
}

/* ocr.lasterr() -> str — prose, for a human. */
int64_t tk_ocr_lasterr_w(void)
{
    const char *m = ocr_lasterr();
    return (int64_t)(intptr_t)(m ? m : "");
}

/* ocr.lasterrkind() -> str — one of "none", "noengine", "badimage",
 * "toosmall", "nolanguage", "failed", "io".
 *
 * Separate from the message for the reason std.pdf keeps them separate: a
 * caller that has to substring-match a prose sentence to tell "no engine on
 * this platform" from "that file is not an image" is one rewording away
 * from treating a scanned statement as an empty one.  "noengine" is the
 * distinct kind requirement 5 of this story asks for. */
int64_t tk_ocr_lasterrkind_w(void)
{
    const char *k = ocr_lasterr_kind();
    return (int64_t)(intptr_t)(k ? k : "none");
}
