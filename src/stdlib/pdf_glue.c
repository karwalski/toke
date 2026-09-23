/*
 * pdf_glue.c — i64-ABI wrappers for the std.pdf module (story 135.3).
 *
 * Registered against module "pdf" in src/stdlib_deps.c.  136.33 is why that
 * sentence is here: a glue file registered under the wrong module name
 * leaves the module unlinkable ON ITS OWN, and nothing shows it up until a
 * program imports that module and nothing else.  test/conform/C032 compiles
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
 * §5 is the one that bites, and it bites harder here than it did for
 * std.xlsx because FIVE of the seven slots are f64.  An f64 field travels
 * as the BIT PATTERN of the double in an i64 slot — `is_f64_field()` in
 * src/llvm.c is what makes the toke side read it back as a number — so a
 * slot written with `(int64_t)value` instead of `f64_to_i64(value)` yields
 * the integer 72 where the page says 72.0, or 0 where it says 0.5.  That
 * compiles, runs, and prints a plausible number.
 *
 * A `$textrun` built here MUST have exactly the seven slots stdlib/pdf.tki
 * declares, in that order:
 *
 *     slot 0  text      str   NUL-terminated UTF-8
 *     slot 1  page      i64   plain integer
 *     slot 2  x         f64   bit pattern
 *     slot 3  y         f64   bit pattern
 *     slot 4  width     f64   bit pattern
 *     slot 5  height    f64   bit pattern
 *     slot 6  fontsize  f64   bit pattern
 *
 * C032 reads every one of those fields and asserts its value against what
 * the PRODUCER put in the file, which is the only defence against the shape
 * silently drifting from the .tki.
 */

#include "pdf.h"
#include "tk_array.h"
#include "bytes_rt.h"
#include "capabilities.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Number of i64 slots in a $textrun — keep in sync with stdlib/pdf.tki. */
#define TK_TEXTRUN_SLOTS 7

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

/* pdf.open(data:@(byte)) -> $pdfdoc!$pdferr
 *
 * 0 on any rejection, which is what the compiled error-union ABI lowers the
 * `$err` arm to.  pdf.lasterrkind() carries WHICH rejection — in particular
 * "encrypted", which story 135.3 requires be distinguishable from "no
 * text". */
int64_t tk_pdf_open_w(int64_t data)
{
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    TkPdfDoc *doc;
    if (!buf) return 0;
    doc = pdf_open_mem(buf, n);
    free(buf);
    return (int64_t)(intptr_t)doc;
}

/* pdf.openfile(path:str) -> $pdfdoc!$pdferr
 *
 * The route a large document must take: a toke `@(byte)` costs eight bytes
 * per byte, so `pdf.open` on a 20 MiB report wants 160 MiB of toke array
 * before a glyph is decoded.  Gated on fs.read like every other filesystem
 * sink (124.4c). */
int64_t tk_pdf_openfile_w(int64_t path)
{
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    return (int64_t)(intptr_t)pdf_open_file((const char *)(intptr_t)path);
}

/* pdf.pagecount(doc:$pdfdoc) -> i64 */
int64_t tk_pdf_pagecount_w(int64_t handle)
{
    return (int64_t)pdf_page_count((const TkPdfDoc *)(intptr_t)handle);
}

/* pdf.pagewidth(doc:$pdfdoc; page:i64) -> f64 — /MediaBox width in points. */
int64_t tk_pdf_pagewidth_w(int64_t handle, int64_t page)
{
    return f64_to_i64(pdf_page_width((TkPdfDoc *)(intptr_t)handle, (uint32_t)page));
}

/* pdf.pageheight(doc:$pdfdoc; page:i64) -> f64
 *
 * Exposed because PDF's y axis points UP from the bottom of the page, so a
 * consumer converting a run's `y` to a top-down coordinate needs the page
 * height — and assuming A4 is wrong for every US document, silently. */
int64_t tk_pdf_pageheight_w(int64_t handle, int64_t page)
{
    return f64_to_i64(pdf_page_height((TkPdfDoc *)(intptr_t)handle, (uint32_t)page));
}

/* One $textrun as a flat seven-slot block. */
static int64_t build_run(const TkPdfRun *r)
{
    int64_t *slot = (int64_t *)malloc(TK_TEXTRUN_SLOTS * sizeof(int64_t));
    int64_t text;
    if (!slot) return 0;
    text = dup_str(r->text);
    if (!text) { free(slot); return 0; }
    slot[0] = text;                    /* .text     */
    slot[1] = r->page;                 /* .page     */
    slot[2] = f64_to_i64(r->x);        /* .x        */
    slot[3] = f64_to_i64(r->y);        /* .y        */
    slot[4] = f64_to_i64(r->width);    /* .width    */
    slot[5] = f64_to_i64(r->height);   /* .height   */
    slot[6] = f64_to_i64(r->fontsize); /* .fontsize */
    return (int64_t)(intptr_t)slot;
}

/* pdf.runs(doc:$pdfdoc; page:i64) -> @($textrun)!$pdferr
 *
 * In CONTENT-STREAM ORDER, which is frequently not reading order.  That is
 * deliberate: reading order is derivable from the positions and the
 * emission order is not derivable from anything, so returning the file's
 * order loses nothing and hides nothing.  `pdf.pagetext` is the one that
 * sorts. */
int64_t tk_pdf_runs_w(int64_t handle, int64_t page)
{
    TkPdfDoc *doc = (TkPdfDoc *)(intptr_t)handle;
    TkPdfPage *pg;
    uint32_t n, built = 0;
    int64_t arr;
    int64_t *block;

    if (!doc) return 0;
    pg = pdf_page_runs(doc, (uint32_t)page);
    if (!pg) return 0;

    n = pdf_run_count(pg);
    arr = tk_arr_alloc((int64_t)n, (int64_t)n);
    if (!arr) { pdf_page_free(pg); return 0; }
    block = (int64_t *)(intptr_t)arr;

    for (uint32_t i = 0; i < n; i++) {
        const TkPdfRun *r = pdf_run_at(pg, i);
        int64_t v;
        if (!r) break;
        v = build_run(r);
        if (!v) break;
        block[built++] = v;
    }
    tk_arr_setlen(arr, (int64_t)built);
    pdf_page_free(pg);
    return arr;
}

/* pdf.pagetext(doc:$pdfdoc; page:i64) -> $str!$pdferr — reading order. */
int64_t tk_pdf_pagetext_w(int64_t handle, int64_t page)
{
    char *t = pdf_page_text((TkPdfDoc *)(intptr_t)handle, (uint32_t)page);
    int64_t out;
    if (!t) return 0;
    out = dup_str(t);
    free(t);
    return out;
}

/* pdf.hastext(doc:$pdfdoc; page:i64) -> bool
 *
 * The scanned-document signal.  page == 0 asks about the whole document.
 * A caller routes to OCR only where this is false, which is the point:
 * extraction is exact and OCR is probabilistic, so running OCR over a
 * document that has a text layer pays an error rate it never needed. */
int64_t tk_pdf_hastext_w(int64_t handle, int64_t page)
{
    return pdf_page_has_text((TkPdfDoc *)(intptr_t)handle, (uint32_t)page) ? 1 : 0;
}

/* pdf.close(doc:$pdfdoc) -> void — releases the document and its objects. */
int64_t tk_pdf_close_w(int64_t handle)
{
    pdf_close((TkPdfDoc *)(intptr_t)handle);
    return 0;
}

/* pdf.lasterr() -> str — prose, for a human. */
int64_t tk_pdf_lasterr_w(void)
{
    const char *m = pdf_lasterr();
    return (int64_t)(intptr_t)(m ? m : "");
}

/* pdf.lasterrkind() -> str — one of "none", "baddoc", "encrypted",
 * "nopage", "toolarge", "unsupported", "io".
 *
 * Separate from the message on purpose.  The error-union ABI binds nothing
 * in the `$err` arm, and story 135.3 requires an encrypted document to be
 * DISTINGUISHABLE rather than merely reported: a caller that has to
 * substring-match a prose sentence to decide whether to prompt for a
 * password is one rewording away from silently treating a protected
 * statement as an empty one. */
int64_t tk_pdf_lasterrkind_w(void)
{
    const char *k = pdf_lasterr_kind();
    return (int64_t)(intptr_t)(k ? k : "none");
}
