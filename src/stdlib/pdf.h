/*
 * pdf.h — C interface for the std.pdf standard library module.
 *
 * Extracts TEXT RUNS WITH POSITIONS from a PDF.  Positions are the point:
 * a bank statement is a table, and a flat string has thrown the columns
 * away before the caller ever sees it.
 *
 * ROUTE.  A purpose-built C99 extractor, not a vendored engine.  The full
 * costing is docs/decisions/135.3-pdf-extraction-route.md; the short form is
 * that ADR-0015 disqualifies every C++ candidate (pdfium, podofo, poppler),
 * the only mostly-C engine (MuPDF) is AGPL against this repository's
 * Apache-2.0 and would relicense every binary a user compiles, and there is
 * no permissively-licensed C99 extractor to find.  The subset actually
 * needed — parse the objects, inflate the Flate streams, walk the content
 * stream for the text-showing operators, apply the font's encoding map — is
 * two orders of magnitude smaller than an engine that is almost all
 * rasteriser.
 *
 * zlib is used for FlateDecode and ADDS NO DEPENDENCY: `LDLIBS` is already
 * `-lm -lz -lpthread` (Makefile:68).  miniz is deliberately not used — it is
 * #included directly into zip.c as a single translation unit with
 * MINIZ_NO_DEFLATE_APIS set locally, so a second consumer would be a second
 * copy of those symbols.
 *
 * ── THE FOUR WAYS THIS GOES PLAUSIBLY WRONG ─────────────────────────────
 *
 * Every one of these produces output that LOOKS like text.  That is why
 * test/conform/C032 asserts exact strings at exact positions and never on
 * "something came out".
 *
 *   1. FONT ENCODING.  A byte in a content stream is a glyph code, not a
 *      character.  What it means is decided by /ToUnicode, or by
 *      /Encoding /Differences over a base encoding, or by the font's
 *      built-in encoding — in that order.  Read the bytes directly and a
 *      WinAnsi document comes out right and every other one comes out
 *      subtly wrong.  Handled in pdftext.c, font_decode().
 *
 *   2. LIGATURES.  One glyph code can be several characters: `fi` is
 *      U+FB01 in the standard encodings and maps through /ToUnicode to the
 *      two characters "fi".  A one-code-one-character reader silently
 *      drops or mangles them.  tk_pdf_glyphs carries the code point and the
 *      CMap carries multi-character targets; both paths produce UTF-8.
 *
 *   3. NO MAPPING AT ALL.  An embedded subset font with no /ToUnicode
 *      contains no information about what its codes mean.  A full renderer
 *      does no better except by heuristic.  The defence here is not
 *      cleverness but NOISE: an unmappable code is emitted as U+FFFD, so
 *      the failure is visible in the output rather than plausible.
 *
 *   4. READING ORDER.  Text objects are emitted in whatever order the
 *      producer chose, which is frequently not reading order.  Runs are
 *      returned in CONTENT-STREAM ORDER, deliberately and documented, and
 *      pdf_page_text() is the one that sorts — because sorting needs the
 *      positions, which is the argument for having them.
 *
 * ── WIDTHS ──────────────────────────────────────────────────────────────
 *
 * A standard-14 font may omit /Widths entirely and producers routinely do,
 * so the AFM metrics are compiled in (pdffont.c, generated).  Without them
 * every run's width is a guess, and a wrong width is how a two-column
 * layout is read as one.
 *
 * ── SCOPE ───────────────────────────────────────────────────────────────
 *
 * Text and positions.  NO embedded image extraction (story 135.3 item 5,
 * deliberately not shipped — see the route note), no rendering, no
 * decryption, no OCR.  An encrypted document is refused as a DISTINCT
 * error, because returning empty text reads as "no content", which is a
 * different and wrong answer.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Every allocation belongs to the TkPdfDoc and is released
 * by pdf_close().
 *
 * Story: 135.3
 */

#ifndef TK_STDLIB_PDF_H
#define TK_STDLIB_PDF_H

#include <stddef.h>
#include <stdint.h>

/* ── Limits ──────────────────────────────────────────────────────────────
 *
 * Compile-time on purpose, for the reason zip.h and xlsx.h give: a bound
 * settable from toke is under the control of the code path that is handling
 * the hostile input.  Every one of these bounds something a single
 * honest-looking file can make this module allocate or loop over.
 */

/* Whole file, in bytes.  A PDF larger than this is refused unread.  256 MiB
 * is past any statement, invoice or report and still fits a scan of the
 * file in memory alongside its inflated streams. */
#define TK_PDF_MAX_FILE        (256u * 1024u * 1024u)

/* Distinct object numbers indexed from one file. */
#define TK_PDF_MAX_OBJECTS     2000000u

/* Objects packed into one /ObjStm. */
#define TK_PDF_MAX_OBJSTM      65536u

/* Pages in the page tree.  Also bounds the tree walk, which a /Kids cycle
 * would otherwise turn into a non-terminating one. */
#define TK_PDF_MAX_PAGES       65536u

/* Array/dictionary nesting depth while parsing one object. */
#define TK_PDF_MAX_DEPTH       64u

/* Form XObject recursion inside a content stream.  A form that draws itself
 * is legal syntax and an infinite loop. */
#define TK_PDF_MAX_FORMDEPTH   8u

/* Bytes any one stream may inflate to.  The compression ratio a zip bomb
 * needs is available here too: 128 MiB from a few kilobytes is easy. */
#define TK_PDF_MAX_STREAM      (128u * 1024u * 1024u)

/* Bytes of concatenated, decoded content stream for ONE page. */
#define TK_PDF_MAX_CONTENT     (128u * 1024u * 1024u)

/* Text runs from one page. */
#define TK_PDF_MAX_RUNS        200000u

/* UTF-8 bytes in one run's text.  A run is one text-showing operator, so
 * this bounds one Tj/TJ, not the page. */
#define TK_PDF_MAX_RUNBYTES    8192u

/* Entries in one /ToUnicode CMap. */
#define TK_PDF_MAX_CMAP        65536u

/* Distinct fonts cached for one document. */
#define TK_PDF_MAX_FONTS       4096u

/* Entries in a /Widths or /W array. */
#define TK_PDF_MAX_WIDTHS      65536u

/* ── Error kinds ─────────────────────────────────────────────────────────
 *
 * These are the strings pdf_lasterr_kind() returns and `pdf.lasterrkind`
 * reports.  They are a CLOSED, stable set: switching on them is the
 * intended use.
 *
 * The compiled error-union ABI lowers `T!E` to a null check, so the `$err`
 * arm binds nothing a consumer can read — the same problem zip.lasterr and
 * xlsx.lasterr work around with a message.  A message is not enough here.
 * Story 135.3's third priority is that an ENCRYPTED document must be
 * distinguishable as such, and distinguishing it by substring match on a
 * human-readable sentence is exactly the fragile thing a caller should not
 * have to do.  So the kind is separate from the message and is machine
 * readable.
 */
#define TK_PDF_E_NONE        "none"
#define TK_PDF_E_BADDOC      "baddoc"      /* not a PDF, or unparseable    */
#define TK_PDF_E_ENCRYPTED   "encrypted"   /* /Encrypt present; not opened */
#define TK_PDF_E_NOPAGE      "nopage"      /* page index out of range      */
#define TK_PDF_E_TOOLARGE    "toolarge"    /* a cap above was exceeded     */
#define TK_PDF_E_UNSUPPORTED "unsupported" /* a filter we do not implement */
#define TK_PDF_E_IO          "io"          /* the file could not be read   */

/* ── Types ───────────────────────────────────────────────────────────── */

/*
 * One text run: the text of ONE text-showing operator (Tj, TJ, ' or "),
 * with where it sits on the page.
 *
 * WHY ONE OPERATOR IS THE UNIT.  It is the producer's own unit of text — a
 * table cell is one drawString is one Tj — so it preserves exactly the
 * column structure that makes a statement readable, without this module
 * having to guess where a column boundary is.  Merging runs would throw
 * that away; splitting finer would invent boundaries the file does not
 * contain.
 *
 * COORDINATES are PDF default user space for the page: points (1/72"),
 * origin at the BOTTOM-LEFT of the page's /MediaBox, y increasing UPWARD.
 * They are not flipped to screen convention, because the convention a
 * caller wants is the caller's business and flipping silently is how
 * everything ends up upside down.
 *
 *   x, y     the pen position at the first glyph.  `y` is the BASELINE,
 *            not the bottom of the box — it is what the producer passed to
 *            its own drawString, so it is the number a fixture can assert
 *            exactly.
 *   width    the run's total advance, along the baseline direction.
 *   height   the em box: (ascent - descent) x fontsize, from the font
 *            descriptor where the file has one and 1.0 em where it does
 *            not.  The box is therefore
 *                [x, x + width] x [y + descent, y + descent + height]
 *            with descent negative.
 *   fontsize the effective size after the text and current transformation
 *            matrices, NOT the raw Tf operand — a document that sets a 1pt
 *            font and scales it by 12 in Tm is showing 12pt text.
 *
 * Rotated or skewed text: x, y and fontsize remain exact, and width is
 * measured along the baseline rather than along the page's x axis.  There
 * is no axis-aligned bounding box for rotated text and pretending there is
 * one would be a fifth plausible wrong answer.
 */
typedef struct {
    char   *text;      /* UTF-8, NUL-terminated; never NULL              */
    int64_t page;      /* 1-based page number                            */
    double  x;         /* points from the left of the MediaBox           */
    double  y;         /* points from the bottom; the BASELINE           */
    double  width;     /* advance width in points                        */
    double  height;    /* em box height in points                        */
    double  fontsize;  /* effective size in points                       */
} TkPdfRun;

/* Opaque document handle.  Definition lives in pdf.c. */
typedef struct TkPdfDoc TkPdfDoc;

/* A page's extracted runs, in content-stream order. */
typedef struct TkPdfPage TkPdfPage;

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * pdf_open_mem — open a document held in memory.
 *
 * Builds the object index, resolves the page tree, and REFUSES an encrypted
 * document with TK_PDF_E_ENCRYPTED.  No page content is decoded here: a
 * 400-page report should cost one page's work when one page is wanted.
 *
 * Returns NULL on any failure; pdf_lasterr() says what happened in prose
 * and pdf_lasterr_kind() says it in one of the closed set above.
 */
TkPdfDoc *pdf_open_mem(const uint8_t *data, uint64_t len);

/*
 * pdf_open_file — read `path` and hand it to pdf_open_mem.
 *
 * The route a large document must take, for the reason xlsx.h gives: a toke
 * `@(byte)` stores one byte per i64 slot, so a 20 MiB PDF routed through
 * `pdf.open` costs 160 MiB of toke array before a glyph is decoded.
 */
TkPdfDoc *pdf_open_file(const char *path);

/* Release the document and everything derived from it.  NULL is a no-op. */
void pdf_close(TkPdfDoc *doc);

/* Number of pages.  0 for a NULL document. */
uint32_t pdf_page_count(const TkPdfDoc *doc);

/* /MediaBox width and height of page `n` (1-based) in points, 0 if out of
 * range.  A caller converting to a top-down coordinate system needs the
 * height, and guessing "it is A4" is wrong for every US document. */
double pdf_page_width(TkPdfDoc *doc, uint32_t n);
double pdf_page_height(TkPdfDoc *doc, uint32_t n);

/*
 * pdf_page_runs — decode page `n` (1-based) into text runs.
 *
 * Returns NULL on failure.  A page with no text yields a page with ZERO
 * runs, never NULL, so "scanned" and "failed" are never the same answer —
 * which is the whole point of pdf_page_has_text().
 *
 * The caller frees with pdf_page_free().
 */
TkPdfPage *pdf_page_runs(TkPdfDoc *doc, uint32_t n);
void       pdf_page_free(TkPdfPage *pg);

uint32_t         pdf_run_count(const TkPdfPage *pg);
const TkPdfRun  *pdf_run_at(const TkPdfPage *pg, uint32_t i);

/*
 * pdf_page_has_text — the scanned-document signal (story 135.3, item 4).
 *
 * 1 when page `n` yields at least one run with non-whitespace text.  Pass
 * n == 0 to ask about the whole document, which stops at the first page
 * that has text.
 *
 * This is what routes a document to OCR or away from it.  Extraction is
 * exact and OCR is probabilistic, so running OCR over a document that has a
 * text layer means paying an error rate that the document never required.
 * A page that renders glyphs from a font with no usable mapping counts as
 * HAVING text — it has a text layer, and OCR is not the remedy for a
 * missing /ToUnicode.
 */
int pdf_page_has_text(TkPdfDoc *doc, uint32_t n);

/*
 * pdf_page_text — page `n` as one string, in READING ORDER.
 *
 * The convenience that the run list deliberately is not.  Runs are sorted
 * top-to-bottom then left-to-right, grouped into lines by baseline
 * proximity, joined with a space where the horizontal gap exceeds a quarter
 * em, and separated by '\n' between lines.
 *
 * It sorts because the positions make sorting possible; a reader that
 * concatenated content-stream order would return scrambled text for every
 * document whose producer emitted its text objects out of order, and would
 * look completely fine on every document whose producer did not.
 *
 * Returns a malloc'd NUL-terminated UTF-8 string the caller frees, or NULL
 * on failure.
 */
char *pdf_page_text(TkPdfDoc *doc, uint32_t n);

/*
 * pdf_lasterr / pdf_lasterr_kind — why the most recent pdf_* call failed.
 *
 * kind is one of the TK_PDF_E_* strings and is what a program should
 * switch on; the message is prose for a human and is not stable.
 * Both return "" / TK_PDF_E_NONE when nothing has failed.
 */
const char *pdf_lasterr(void);
const char *pdf_lasterr_kind(void);

#endif /* TK_STDLIB_PDF_H */
