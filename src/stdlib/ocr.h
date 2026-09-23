/*
 * ocr.h — C interface for the std.ocr standard library module (story 135.6).
 *
 * Recognises TEXT RUNS WITH POSITIONS in a raster image, using the operating
 * system's own text recogniser.
 *
 * ── ROUTE ───────────────────────────────────────────────────────────────
 *
 * macOS Vision, bound from plain C99 through the Objective-C runtime C API
 * (objc_getClass / objc_msgSend), exactly as src/stdlib/webview.c binds
 * WebKit.  No .m file, no -x objective-c, no C++ translation unit, so
 * ADR-0015 is untouched: the whole cost is this file and a handful of
 * -framework flags in src/stdlib_deps.c.
 *
 * The full costing, and the probe measurements it rests on, are in
 * docs/decisions/135.6-platform-ocr-route.md.  ADR-0015 refuses vendoring
 * Tesseract (story 135.7) because its C API wraps a C++ core, which leaves
 * the platform recogniser or a standalone component (story 135.8).
 *
 * PORTABILITY IS POOR BY CONSTRUCTION and that is the accepted trade in the
 * story row: a different engine per platform, and none on bare Linux.  The
 * mitigation is the interface, not the implementation.  Nothing a caller
 * touches spells "Vision"; ocr_engine() names whoever answered, and
 * ocr_is_available() says whether anyone can, so story 135.8 can be added as
 * a second engine behind these same calls.
 *
 * ── THE THREE WAYS THIS GOES PLAUSIBLY WRONG ────────────────────────────
 *
 *   1. CONFIDENCE IS A CONSTANT, NOT A MEASUREMENT.  Measured across 40
 *      images on macOS 26.5.1: every recognised candidate scores exactly
 *      1.0 at revision 3 / accurate, including "|||||1100" for an image
 *      reading `Illlll1I0O` and "VOME" for one reading `ACME`.  At fast
 *      level every candidate scores exactly 0.5.  It is a label for how the
 *      request was configured.  A caller that flags a field on
 *      `run.confidence < 0.9` flags NOTHING.  The field is surfaced anyway,
 *      unaveraged, because it is what the platform reports and a future
 *      revision may put information in it — but docs/stdlib/ocr.md says
 *      this in those words so that nobody builds a threshold on it.
 *
 *   2. AN UNRECOGNISED SELECTOR IS FATAL.  C has no @try.  Sending a
 *      selector Vision does not implement raises NSInvalidArgumentException
 *      and terminates the process with no recovery path — this actually
 *      happened during probing, on -[NSIndexSet allObjects].  So EVERY
 *      class is fetched with objc_getClass and EVERY selector is confirmed
 *      with class_respondsToSelector before it is sent.  vision_probe()
 *      below is that check, and ocr_is_available() is the same check
 *      exposed, which is why it is honest rather than a hard-coded 1.
 *
 *   3. EMPTY IS NOT UNAVAILABLE.  Returning zero runs on a platform with no
 *      recogniser reads to a caller as "this page has no text", which is
 *      the same defect class std.pdf handles by refusing an encrypted
 *      document with a DISTINCT $encrypted rather than returning "".  Every
 *      entry point here fails with OCR_ERR_NOENGINE when there is no
 *      engine, and ocr_lasterr_kind() carries which.
 *
 * ── COORDINATES ─────────────────────────────────────────────────────────
 *
 * ORIGIN BOTTOM-LEFT, Y INCREASING UPWARD, UNIT = PIXELS OF THE SOURCE
 * IMAGE.  This follows std.pdf rather than the raster convention, on
 * purpose: the consumer this epic exists for routes on pdf.hastext, sends
 * text pages to pdf.runs and scanned pages here, and feeds BOTH into one
 * field extractor.  Vision's normalised boundingBox already uses the
 * bottom-left origin, so only the unit is converted.
 *
 * TkOcrRun is laid out as std.pdf's TkPdfRun is, field for field, so the
 * two are bit-identical under the flat-i64-block struct ABI.  The single
 * divergence is the last field: a PDF knows the font size and not the
 * confidence, a recogniser knows the confidence and not the font size.
 * Keeping `fontsize` and writing 0.0 into it was rejected — a field that is
 * a lie in every row is worse than a field that is absent.
 *
 * Story: 135.6
 */

#ifndef TK_OCR_H
#define TK_OCR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error kinds ────────────────────────────────────────────────────────
 *
 * Mirrors the $ocrerr sum in stdlib/ocr.tki.  Kept as an enum rather than
 * bare strings so that a new kind cannot be added in one place only.
 */
typedef enum {
    OCR_ERR_NONE = 0,
    OCR_ERR_NOENGINE,    /* no recogniser on this platform — NOT "no text" */
    OCR_ERR_BADIMAGE,    /* bytes are not a decodable image */
    OCR_ERR_TOOSMALL,    /* below the engine's minimum usable size */
    OCR_ERR_NOLANGUAGE,  /* a requested language the engine does not have */
    OCR_ERR_FAILED,      /* the engine itself returned an error */
    OCR_ERR_IO           /* the file could not be read */
} TkOcrErr;

/* ── One recognised run ─────────────────────────────────────────────────
 *
 * Field order is normative: src/stdlib/ocr_glue.c writes one i64 slot per
 * field in THIS order, and stdlib/ocr.tki declares the same seven in the
 * same order.  Changing the order here without changing both is how a
 * caller reads a confidence where it asked for a width.
 */
typedef struct {
    char   *text;        /* UTF-8, owned by the TkOcrPage                 */
    int64_t page;        /* 1-based; always 1 for a single image          */
    double  x;           /* pixels from the LEFT   of the source image    */
    double  y;           /* pixels from the BOTTOM of the source image    */
    double  width;       /* pixels                                        */
    double  height;      /* pixels                                        */
    double  confidence;  /* as reported; see warning 1 above              */
} TkOcrRun;

/* Opaque result set.  Free with ocr_page_free(). */
typedef struct TkOcrPage TkOcrPage;

/* ── Availability ───────────────────────────────────────────────────────
 *
 * A REAL PROBE, not a constant.  On macOS it confirms that both Vision
 * classes resolve at runtime, that every selector this module sends is
 * implemented, and that the engine will actually answer
 * +supportedRecognitionLanguagesAndReturnError: with a non-empty list —
 * which is what fails when the text model is not installed.  Everywhere
 * else it is 0 because there is nothing to probe.
 */
int         ocr_is_available(void);

/* "vision" on macOS when available, "none" otherwise.  The seam story 135.8
 * would extend, and the reason no caller has to name a platform. */
const char *ocr_engine(void);

/* Comma-separated BCP-47 tags the engine actually supports, queried live.
 * "" when there is no engine.  Never a compiled-in list. */
const char *ocr_languages(void);

/* ── Configuration ──────────────────────────────────────────────────────
 *
 * Process-wide and deliberately minimal.  Returns 0 and sets the last error
 * to OCR_ERR_NOLANGUAGE if any tag is one the engine does not support,
 * rather than silently recognising in the wrong language.
 * `langs` is a comma-separated BCP-47 list; NULL or "" restores the default.
 */
int  ocr_set_languages(const char *langs);

/* 0 = accurate (the default), 1 = fast.  Fast is roughly an order of
 * magnitude quicker and, in the probe sweep, returned NOTHING for every
 * input accurate got wrong — it refuses rather than guesses. */
void ocr_set_fast(int fast);

/* ── Recognition ────────────────────────────────────────────────────────
 *
 * All three decode to a CGImage and run one request.  NULL on failure, with
 * ocr_lasterr_kind() carrying which failure.  A page with no text at all is
 * a SUCCESS with zero runs — that is distinct from a failure and a caller
 * depends on the difference.
 */

/* Encoded image bytes: PNG, JPEG, TIFF, BMP, GIF, HEIC — whatever ImageIO
 * on the host can decode. */
TkOcrPage *ocr_recognize_mem(const uint8_t *bytes, uint64_t len);

/* A path.  The route a large scan must take: a toke @(byte) costs eight
 * bytes per byte, so a 20 MB TIFF wants 160 MB of toke array before a
 * pixel is decoded.  Capability-gated at the glue, like every fs sink. */
TkOcrPage *ocr_recognize_file(const char *path);

/* Raw pixels, the composition point with std.image: greyscale, crop,
 * rotate and adaptive-threshold there, then recognise here without a
 * re-encode.  `channels` is 1 (grey), 3 (RGB) or 4 (RGBA); rows are tightly
 * packed, TOP row first, which is std.image's layout. */
TkOcrPage *ocr_recognize_raw(const uint8_t *pixels, uint32_t width,
                             uint32_t height, uint32_t channels);

uint32_t        ocr_run_count(const TkOcrPage *page);
const TkOcrRun *ocr_run_at(const TkOcrPage *page, uint32_t index);

/* All runs joined with '\n' in the engine's own order.  Caller frees.
 * NOT sorted: see docs/stdlib/ocr.md — Vision returns top-to-bottom for the
 * single-column case this module was measured on, and inventing a
 * multi-column sort here would be a heuristic pretending to be a fact. */
char *ocr_page_text(const TkOcrPage *page);

void ocr_page_free(TkOcrPage *page);

/* ── Last error ─────────────────────────────────────────────────────────
 *
 * Kind and message are separate for the reason std.pdf keeps them separate:
 * a caller that has to substring-match a prose sentence to tell "no engine
 * on this platform" from "that file is not an image" is one rewording away
 * from treating a scanned statement as an empty one.
 */
const char *ocr_lasterr(void);       /* prose, for a human          */
const char *ocr_lasterr_kind(void);  /* "none" | "noengine" | ...   */
TkOcrErr    ocr_lasterr_code(void);

#ifdef __cplusplus
}
#endif

#endif /* TK_OCR_H */
