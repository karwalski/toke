/*
 * ocr.c — Implementation of the std.ocr standard library module (story 135.6).
 *
 * Platform support:
 *   macOS  — Vision's VNRecognizeTextRequest, reached through the
 *            Objective-C runtime C API (objc_getClass / objc_msgSend) so
 *            this file compiles as plain C99 without -x objective-c.  Same
 *            technique as src/stdlib/webview.c, which binds WebKit.
 *   Windows — reserved for the Windows.Media.Ocr API; NOT implemented, and
 *            deliberately not stubbed with plausible values.  See below.
 *   Other  — no system recogniser exists; every call fails NOENGINE.
 *
 * On macOS link with:
 *   -framework Vision -framework CoreGraphics -framework ImageIO
 *   -framework CoreFoundation -framework Foundation -lobjc
 * No -x objective-c flag required; plain cc -std=c99 suffices.  The probe
 * that established this, and the confidence measurements that shaped the
 * interface, are in docs/decisions/135.6-platform-ocr-route.md.
 *
 * WHY WINDOWS IS ABSENT RATHER THAN STUBBED.  The story row names the
 * Windows OCR API and this machine cannot test it.  An untested platform
 * branch that returns plausible values is precisely the defect Epic 136
 * found 31 times (check-glue-core exists because of it), so the Windows
 * path returns NOENGINE and says so.  A caller on Windows learns there is
 * no recogniser, which is true, instead of learning the page is blank,
 * which is not.
 *
 * Story: 135.6
 */

#include "ocr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════
 * Shared: the page object and the last-error record
 *
 * Both halves of the #ifdef build the same TkOcrPage, so the accessors,
 * ocr_page_text() and ocr_page_free() are written once.
 * ══════════════════════════════════════════════════════════════════════ */

struct TkOcrPage {
    TkOcrRun *runs;
    uint32_t  count;
};

static TkOcrErr    g_err_code = OCR_ERR_NONE;
static char        g_err_msg[512] = "";

static const char *err_kind_name(TkOcrErr e)
{
    switch (e) {
    case OCR_ERR_NONE:       return "none";
    case OCR_ERR_NOENGINE:   return "noengine";
    case OCR_ERR_BADIMAGE:   return "badimage";
    case OCR_ERR_TOOSMALL:   return "toosmall";
    case OCR_ERR_NOLANGUAGE: return "nolanguage";
    case OCR_ERR_FAILED:     return "failed";
    case OCR_ERR_IO:         return "io";
    }
    return "none";
}

static void set_err(TkOcrErr code, const char *msg)
{
    g_err_code = code;
    if (msg) {
        size_t n = strlen(msg);
        if (n >= sizeof g_err_msg) n = sizeof g_err_msg - 1;
        memcpy(g_err_msg, msg, n);
        g_err_msg[n] = '\0';
    } else {
        g_err_msg[0] = '\0';
    }
}

const char *ocr_lasterr(void)      { return g_err_msg; }
const char *ocr_lasterr_kind(void) { return err_kind_name(g_err_code); }
TkOcrErr    ocr_lasterr_code(void) { return g_err_code; }

uint32_t ocr_run_count(const TkOcrPage *page)
{
    return page ? page->count : 0u;
}

const TkOcrRun *ocr_run_at(const TkOcrPage *page, uint32_t index)
{
    if (!page || index >= page->count) return NULL;
    return &page->runs[index];
}

void ocr_page_free(TkOcrPage *page)
{
    uint32_t i;
    if (!page) return;
    for (i = 0; i < page->count; i++) free(page->runs[i].text);
    free(page->runs);
    free(page);
}

/*
 * Join every run with '\n', in the engine's own order.
 *
 * NOT sorted.  std.pdf sorts because a PDF's emission order is arbitrary
 * and its positions are exact; here the positions are themselves estimates
 * and the engine already returns a reading order for the single-column case
 * this module was measured on.  A multi-column sort invented here would be
 * a heuristic wearing the costume of a fact.
 */
char *ocr_page_text(const TkOcrPage *page)
{
    size_t total = 1, pos = 0;
    uint32_t i;
    char *out;

    if (!page) return NULL;
    for (i = 0; i < page->count; i++)
        total += (page->runs[i].text ? strlen(page->runs[i].text) : 0) + 1;

    out = (char *)malloc(total);
    if (!out) return NULL;

    for (i = 0; i < page->count; i++) {
        const char *t = page->runs[i].text;
        size_t n = t ? strlen(t) : 0;
        if (i) out[pos++] = '\n';
        if (n) memcpy(out + pos, t, n);
        pos += n;
    }
    out[pos] = '\0';
    return out;
}

/* ══════════════════════════════════════════════════════════════════════
 * macOS — Vision through the Objective-C runtime C API
 * ══════════════════════════════════════════════════════════════════════ */
#ifdef __APPLE__

#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

/*
 * On LP64 (the only macOS ABI we target) NSInteger == ptrdiff_t and
 * NSUInteger == size_t.  Local aliases, as webview.c does, so that
 * NSObjCRuntime.h is not needed and this stays a C translation unit.
 */
typedef ptrdiff_t TkNSInteger;
typedef size_t    TkNSUInteger;

/*
 * Every objc_msgSend goes through a fully-spelled function-pointer cast.
 * No variadic macros: -Wpedantic rejects the GNU extension.
 *
 * ARM64 ABI, and both of these would compile and return nonsense if got
 * wrong, which is why they are written down:
 *   - CGRect is four doubles, a homogeneous float aggregate, returned in
 *     registers.  There is no objc_msgSend_stret on arm64, so the plain
 *     cast is correct.  It would NOT be on i386.
 *   - VNConfidence is a float.  There is no objc_msgSend_fpret on arm64
 *     either, so again the plain cast is correct.
 */
typedef id          (*Msg0)(id, SEL);
typedef id          (*Msg1)(id, SEL, id);
typedef TkNSUInteger(*MsgU0)(id, SEL);
typedef id          (*MsgIdx)(id, SEL, TkNSUInteger);
typedef const char *(*MsgStr)(id, SEL);
typedef float       (*MsgFlt)(id, SEL);
typedef CGRect      (*MsgRect)(id, SEL);
typedef void        (*VoidNI)(id, SEL, TkNSInteger);
typedef void        (*VoidNU)(id, SEL, TkNSUInteger);
typedef void        (*VoidBool)(id, SEL, BOOL);
typedef void        (*VoidId)(id, SEL, id);
typedef id          (*MsgCGImg)(id, SEL, CGImageRef, id);
typedef BOOL        (*MsgPerform)(id, SEL, id, id *);
typedef id          (*MsgErrOut)(id, SEL, TkNSInteger, id *);

static id           ms0(id o, const char *s)  { return ((Msg0)objc_msgSend)(o, sel_getUid(s)); }
static id           ms1(id o, const char *s, id a) { return ((Msg1)objc_msgSend)(o, sel_getUid(s), a); }
static TkNSUInteger msu(id o, const char *s)  { return ((MsgU0)objc_msgSend)(o, sel_getUid(s)); }
static id           msidx(id o, const char *s, TkNSUInteger i) { return ((MsgIdx)objc_msgSend)(o, sel_getUid(s), i); }
static const char  *msutf8(id o)              { return o ? ((MsgStr)objc_msgSend)(o, sel_getUid("UTF8String")) : NULL; }

/* Only the macOS path ever succeeds, so only it ever clears.  Defined here
 * rather than beside set_err() because off Apple nothing calls it and
 * -Werror=unused-function is fatal -- which is how this was found: compiling
 * this file with -U__APPLE__ failed the build, and no gate in this tree
 * compiles a stdlib module .c file for a non-Apple target. */
static void clear_err(void) { g_err_code = OCR_ERR_NONE; g_err_msg[0] = '\0'; }

/*
 * AUTORELEASE POOL.  A C program has no @autoreleasepool block and no pool
 * of its own, so every autoreleased object Vision hands back -- the results
 * array, each observation, each candidate, each NSString -- accumulates for
 * the life of the process.  Measured before this was added: ~47 KB of
 * permanent growth per recognition, which for the consumer this module
 * exists for (a statement is dozens of pages, a batch is hundreds of
 * statements) is a leak that ends in the OOM killer rather than a wrong
 * answer.
 *
 * objc_autoreleasePoolPush/Pop are the C entry points to the same machinery
 * the @autoreleasepool keyword compiles to, and they are what makes this
 * fixable without an Objective-C compiler.  Declared here rather than
 * included: they live in <objc/objc-internal.h>, which is not in the public
 * SDK headers, while the symbols themselves are stable public API in
 * libobjc.
 *
 * EVERY string is copied out of the pool's objects BEFORE the pop.
 */
extern void *objc_autoreleasePoolPush(void);
extern void  objc_autoreleasePoolPop(void *pool);

/* ── Configuration state ────────────────────────────────────────────── */

#define OCR_MAX_LANGS 16

static char g_langs[OCR_MAX_LANGS][32];
static int  g_lang_count = 0;
static int  g_fast = 0;

/* Cached, because ocr_languages() hands back a pointer the caller reads. */
static char g_lang_list[1024] = "";

/* ── Selector safety ────────────────────────────────────────────────────
 *
 * C has no @try.  A selector Vision does not implement raises
 * NSInvalidArgumentException and takes the process with it — that is not a
 * theoretical risk, it happened during probing.  So nothing below is sent
 * until it has been confirmed here, and this same function is what makes
 * ocr_is_available() an honest probe instead of a compiled-in 1.
 */
typedef struct {
    Class handler_cls;   /* VNImageRequestHandler   */
    Class request_cls;   /* VNRecognizeTextRequest  */
} VisionClasses;

static int vision_classes(VisionClasses *out)
{
    static const char *inst_sels[] = {
        "initWithCGImage:options:", "performRequests:error:"
    };
    static const char *req_sels[] = {
        "setRecognitionLevel:", "setUsesLanguageCorrection:",
        "setRecognitionLanguages:", "setRevision:", "results",
        "supportedRecognitionLanguagesAndReturnError:"
    };
    static const char *obs_needed[] = { "topCandidates:", "boundingBox" };
    Class hc, rc, oc;
    size_t i;

    hc = objc_getClass("VNImageRequestHandler");
    rc = objc_getClass("VNRecognizeTextRequest");
    oc = objc_getClass("VNRecognizedTextObservation");
    if (!hc || !rc || !oc) return 0;

    for (i = 0; i < sizeof inst_sels / sizeof *inst_sels; i++)
        if (!class_respondsToSelector(hc, sel_getUid(inst_sels[i]))) return 0;
    for (i = 0; i < sizeof req_sels / sizeof *req_sels; i++)
        if (!class_respondsToSelector(rc, sel_getUid(req_sels[i]))) return 0;
    for (i = 0; i < sizeof obs_needed / sizeof *obs_needed; i++)
        if (!class_respondsToSelector(oc, sel_getUid(obs_needed[i]))) return 0;

    /* VNRecognizedText carries the string and the confidence. */
    {
        Class tc = objc_getClass("VNRecognizedText");
        if (!tc) return 0;
        if (!class_respondsToSelector(tc, sel_getUid("string"))) return 0;
        if (!class_respondsToSelector(tc, sel_getUid("confidence"))) return 0;
    }

    out->handler_cls = hc;
    out->request_cls = rc;
    return 1;
}

/*
 * The recognition revision is PINNED.
 *
 * Revision is not cosmetic: on this machine a clean image scores confidence
 * 1.0 at revision 3 and 0.5 at revision 2, for byte-identical input.  An
 * unpinned request silently follows whatever macOS ships next, so a
 * conformance suite that passes today fails on an OS update for a reason
 * nothing in the diff explains.  Pin to the newest revision this Vision
 * actually supports, and record which in ocr_engine()'s answer.
 */
static TkNSUInteger vision_revision(Class request_cls)
{
    id sr;
    TkNSUInteger want;

    sr = ms0((id)request_cls, "supportedRevisions");
    if (!sr || !class_respondsToSelector(object_getClass(sr),
                                         sel_getUid("containsIndex:")))
        return 0;   /* 0 = do not call setRevision:, take the default */

    for (want = 8; want > 0; want--)
        if (((BOOL (*)(id, SEL, TkNSUInteger))objc_msgSend)(
                sr, sel_getUid("containsIndex:"), want))
            return want;
    return 0;
}

/* ── Availability, honestly ─────────────────────────────────────────────
 *
 * Three checks, each of which has a real failure mode:
 *   1. the classes resolve            — Vision.framework actually linked
 *   2. every selector is implemented  — the API has not moved under us
 *   3. the engine answers with a NON-EMPTY language list — this is the one
 *      that fails when the framework is present but its text model is not
 *      installed, which a header-only check cannot see.
 *
 * std.mlx's isavailable is the counter-example: tk_mlx_isavailable_w()
 * hard-returns 0 in BOTH arms of its #if, ignoring the real mlx_is_available()
 * probe sitting in the same module.  Nothing below is a constant.
 */
int ocr_is_available(void)
{
    VisionClasses vc;
    id req, langs;

    if (!vision_classes(&vc)) return 0;

    req = ms0(ms0((id)vc.request_cls, "alloc"), "init");
    if (!req) return 0;

    langs = ((MsgErrOut)objc_msgSend)(
        req, sel_getUid("supportedRecognitionLanguagesAndReturnError:"), 0, NULL);

    {
        int ok = (langs && msu(langs, "count") > 0) ? 1 : 0;
        ms0(req, "release");
        return ok;
    }
}

const char *ocr_engine(void)
{
    return ocr_is_available() ? "vision" : "none";
}

const char *ocr_languages(void)
{
    VisionClasses vc;
    id req, langs;
    TkNSUInteger n, i;
    size_t pos = 0;

    g_lang_list[0] = '\0';
    if (!vision_classes(&vc)) return g_lang_list;

    req = ms0(ms0((id)vc.request_cls, "alloc"), "init");
    if (!req) return g_lang_list;

    langs = ((MsgErrOut)objc_msgSend)(
        req, sel_getUid("supportedRecognitionLanguagesAndReturnError:"), 0, NULL);
    n = langs ? msu(langs, "count") : 0;

    for (i = 0; i < n; i++) {
        const char *tag = msutf8(ms0(msidx(langs, "objectAtIndex:", i), "self"));
        size_t tl;
        if (!tag) continue;
        tl = strlen(tag);
        if (pos + tl + 2 >= sizeof g_lang_list) break;
        if (pos) g_lang_list[pos++] = ',';
        memcpy(g_lang_list + pos, tag, tl);
        pos += tl;
    }
    g_lang_list[pos] = '\0';
    ms0(req, "release");
    return g_lang_list;
}

/*
 * Whole-item membership in a comma-separated list.  A plain strstr would
 * accept "en" because "en-US" contains it, and Vision silently ignores a
 * tag it does not know -- so the caller would get confident output in the
 * default language having asked for another.
 */
static int list_has_item(const char *list, const char *item, size_t len)
{
    const char *p = list;
    while ((p = strstr(p, item)) != NULL) {
        const char *end = p + len;
        if ((p == list || p[-1] == ',') && (*end == ',' || *end == '\0'))
            return 1;
        p = end;
    }
    return 0;
}

/*
 * Every requested tag is checked against the live list BEFORE it is
 * accepted.  Vision silently ignores an unsupported tag and recognises in
 * its default language instead, which produces confident output in the
 * wrong language — the exact shape of failure this module is trying not to
 * have.
 */
int ocr_set_languages(const char *langs)
{
    char buf[512];
    char accepted[OCR_MAX_LANGS][32];
    int  count = 0, k;
    const char *supported;
    char *tok, *save = NULL;

    clear_err();

    if (!langs || !*langs) { g_lang_count = 0; return 1; }

    if (!ocr_is_available()) {
        set_err(OCR_ERR_NOENGINE,
                "no text recogniser on this platform; nothing to configure");
        return 0;
    }

    if (strlen(langs) >= sizeof buf) {
        set_err(OCR_ERR_NOLANGUAGE, "language list too long");
        return 0;
    }
    strcpy(buf, langs);

    supported = ocr_languages();

    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        size_t tl;
        while (*tok == ' ') tok++;
        tl = strlen(tok);
        while (tl && tok[tl - 1] == ' ') tok[--tl] = '\0';
        if (!tl) continue;

        if (count >= OCR_MAX_LANGS) {
            set_err(OCR_ERR_NOLANGUAGE, "too many languages requested");
            return 0;
        }
        if (tl >= sizeof accepted[0]) {
            set_err(OCR_ERR_NOLANGUAGE, "language tag too long");
            return 0;
        }
        if (!list_has_item(supported, tok, tl)) {
            char msg[128];
            snprintf(msg, sizeof msg,
                     "language '%s' is not supported by this recogniser", tok);
            set_err(OCR_ERR_NOLANGUAGE, msg);
            return 0;
        }
        strcpy(accepted[count++], tok);
    }

    /* Commit only once every tag has been accepted: a half-applied list
     * would recognise in a language the caller never asked for. */
    for (k = 0; k < count; k++) strcpy(g_langs[k], accepted[k]);
    g_lang_count = count;
    return 1;
}

void ocr_set_fast(int fast) { g_fast = fast ? 1 : 0; }

/* ── CGImage construction ───────────────────────────────────────────── */

static CGImageRef image_from_bytes(const uint8_t *bytes, uint64_t len)
{
    CFDataRef data;
    CGImageSourceRef src;
    CGImageRef img;

    if (!bytes || !len) return NULL;
    data = CFDataCreate(NULL, bytes, (CFIndex)len);
    if (!data) return NULL;
    src = CGImageSourceCreateWithData(data, NULL);
    CFRelease(data);
    if (!src) return NULL;
    img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    return img;
}

/* CGDataProvider's release callback: the buffer handed to
 * CGDataProviderCreateWithData is malloc'd by image_from_raw and freed here,
 * once, when CoreGraphics drops its last reference. */
static void free_owned(void *info, const void *data, size_t size)
{
    (void)info;
    (void)size;
    free((void *)(uintptr_t)data);
}

/*
 * Raw pixels, TOP row first (std.image's layout), tightly packed.
 *
 * A greyscale buffer goes through a DeviceGray colour space with
 * kCGImageAlphaNone; 3- and 4-channel buffers go through DeviceRGB.  The
 * three-channel case has no 24-bit bitmap layout in CoreGraphics, so it is
 * widened to RGBA here rather than handed over in a layout CGBitmapContext
 * would reject.
 */
static CGImageRef image_from_raw(const uint8_t *px, uint32_t w, uint32_t h,
                                 uint32_t ch)
{
    CGColorSpaceRef cs;
    CGDataProviderRef prov;
    CGImageRef img;
    uint8_t *owned = NULL;
    size_t bpp, bpr, nbytes;

    if (!px || !w || !h) return NULL;

    if (ch == 3) {
        size_t i, n = (size_t)w * h;
        owned = (uint8_t *)malloc(n * 4);
        if (!owned) return NULL;
        for (i = 0; i < n; i++) {
            owned[i * 4 + 0] = px[i * 3 + 0];
            owned[i * 4 + 1] = px[i * 3 + 1];
            owned[i * 4 + 2] = px[i * 3 + 2];
            owned[i * 4 + 3] = 255;
        }
        ch = 4;
        px = owned;
    }

    if (ch == 1) {
        cs = CGColorSpaceCreateDeviceGray();
        bpp = 8;
    } else if (ch == 4) {
        cs = CGColorSpaceCreateDeviceRGB();
        bpp = 32;
    } else {
        free(owned);
        return NULL;
    }
    if (!cs) { free(owned); return NULL; }

    bpr    = (size_t)w * (bpp / 8);
    nbytes = bpr * h;

    if (!owned) {
        owned = (uint8_t *)malloc(nbytes);
        if (!owned) { CGColorSpaceRelease(cs); return NULL; }
        memcpy(owned, px, nbytes);
    }

    /*
     * The provider takes ownership of `owned` and free_owned() releases it
     * when CoreGraphics is done with it.  Passing NULL here instead -- which
     * this code did until the static analyser flagged the release below --
     * leaks the whole pixel copy on EVERY call, silently, because nothing
     * else in the process ever holds that pointer again.
     *
     * Ownership transfers at the CGDataProviderCreateWithData call that
     * succeeds, so after it there is no `free(owned)` on any path: if
     * CGImageCreate fails, CGDataProviderRelease runs the callback and the
     * buffer is freed exactly once.  Freeing it here as well would be a
     * double free on the failure path.
     */
    prov = CGDataProviderCreateWithData(NULL, owned, nbytes, free_owned);
    if (!prov) { free(owned); CGColorSpaceRelease(cs); return NULL; }

    img = CGImageCreate((size_t)w, (size_t)h, 8, bpp, bpr, cs,
                        (ch == 1) ? kCGImageAlphaNone
                                  : (CGBitmapInfo)kCGImageAlphaNoneSkipLast,
                        prov, NULL, 0, kCGRenderingIntentDefault);
    CGDataProviderRelease(prov);
    CGColorSpaceRelease(cs);
    return img;
}

/* ── The one recognition path ───────────────────────────────────────── */

/*
 * Vision's minimum.  Below roughly this the engine either returns nothing
 * or returns a truncated guess at full confidence ("VOME" for ACME in a
 * 24x10 image, measured).  Refusing outright with TOOSMALL is more useful
 * than passing the guess on, because the guess is indistinguishable from a
 * real reading.
 */
#define OCR_MIN_DIMENSION 20

static TkOcrPage *recognize_cgimage_pooled(CGImageRef img);

static TkOcrPage *recognize_cgimage(CGImageRef img)
{
    void *pool = objc_autoreleasePoolPush();
    TkOcrPage *page = recognize_cgimage_pooled(img);
    objc_autoreleasePoolPop(pool);
    return page;
}

static TkOcrPage *recognize_cgimage_pooled(CGImageRef img)
{
    VisionClasses vc;
    id handler, req, arr, err = NULL, results;
    TkNSUInteger n, i, rev;
    TkOcrPage *page;
    double iw, ih;
    BOOL ok;

    if (!img) {
        set_err(OCR_ERR_BADIMAGE, "the bytes are not a decodable image");
        return NULL;
    }

    iw = (double)CGImageGetWidth(img);
    ih = (double)CGImageGetHeight(img);
    if (iw < OCR_MIN_DIMENSION || ih < OCR_MIN_DIMENSION) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "image is %gx%g; below %dx%d the recogniser returns a guess "
                 "at full confidence rather than a reading",
                 iw, ih, OCR_MIN_DIMENSION, OCR_MIN_DIMENSION);
        set_err(OCR_ERR_TOOSMALL, msg);
        return NULL;
    }

    if (!vision_classes(&vc)) {
        set_err(OCR_ERR_NOENGINE, "no text recogniser is available on this host");
        return NULL;
    }

    handler = ((MsgCGImg)objc_msgSend)(ms0((id)vc.handler_cls, "alloc"),
                                       sel_getUid("initWithCGImage:options:"),
                                       img, NULL);
    if (!handler) {
        set_err(OCR_ERR_FAILED, "the recogniser rejected the image");
        return NULL;
    }

    req = ms0(ms0((id)vc.request_cls, "alloc"), "init");
    if (!req) {
        ms0(handler, "release");
        set_err(OCR_ERR_FAILED, "the recogniser could not create a request");
        return NULL;
    }

    rev = vision_revision(vc.request_cls);
    if (rev) ((VoidNU)objc_msgSend)(req, sel_getUid("setRevision:"), rev);

    ((VoidNI)objc_msgSend)(req, sel_getUid("setRecognitionLevel:"),
                           g_fast ? (TkNSInteger)1 : (TkNSInteger)0);

    /*
     * Language correction OFF by default.  It rewrites recognised text
     * towards dictionary words, which on a bank statement turns account
     * numbers and reference codes into English — a transformation that is
     * invisible in the output and wrong in exactly the fields that matter.
     */
    ((VoidBool)objc_msgSend)(req, sel_getUid("setUsesLanguageCorrection:"), NO);

    if (g_lang_count > 0) {
        id list = ms0((id)objc_getClass("NSMutableArray"), "array");
        int k;
        for (k = 0; k < g_lang_count; k++) {
            id s = ((Msg1)objc_msgSend)((id)objc_getClass("NSString"),
                                        sel_getUid("stringWithUTF8String:"),
                                        (id)(uintptr_t)g_langs[k]);
            if (s) ((VoidId)objc_msgSend)(list, sel_getUid("addObject:"), s);
        }
        ((VoidId)objc_msgSend)(req, sel_getUid("setRecognitionLanguages:"), list);
    }

    arr = ms1((id)objc_getClass("NSArray"), "arrayWithObject:", req);
    ok  = ((MsgPerform)objc_msgSend)(handler, sel_getUid("performRequests:error:"),
                                     arr, &err);

    if (!ok) {
        const char *d = err ? msutf8(ms0(err, "localizedDescription")) : NULL;
        set_err(OCR_ERR_FAILED, d ? d : "the recogniser failed");
        ms0(req, "release");
        ms0(handler, "release");
        return NULL;
    }

    results = ms0(req, "results");
    n = results ? msu(results, "count") : 0;

    page = (TkOcrPage *)calloc(1, sizeof *page);
    if (!page) {
        set_err(OCR_ERR_FAILED, "out of memory");
        ms0(req, "release");
        ms0(handler, "release");
        return NULL;
    }
    if (n) {
        page->runs = (TkOcrRun *)calloc((size_t)n, sizeof *page->runs);
        if (!page->runs) {
            free(page);
            set_err(OCR_ERR_FAILED, "out of memory");
            ms0(req, "release");
            ms0(handler, "release");
            return NULL;
        }
    }

    for (i = 0; i < n; i++) {
        id obs = msidx(results, "objectAtIndex:", i);
        id cands, cand, str;
        CGRect bb;
        const char *utf8;
        TkOcrRun *r;

        if (!obs) continue;

        /*
         * topCandidates:1 — asked for 3 or 5 during probing, revision 3
         * returned exactly one every time, so asking for more buys nothing
         * but allocation.  The count is still checked: a future revision
         * returning zero must not be read as a candidate.
         */
        cands = ((MsgIdx)objc_msgSend)(obs, sel_getUid("topCandidates:"),
                                       (TkNSUInteger)1);
        if (!cands || msu(cands, "count") == 0) continue;

        cand = msidx(cands, "objectAtIndex:", 0);
        if (!cand) continue;

        str  = ms0(cand, "string");
        utf8 = msutf8(str);
        if (!utf8) continue;

        bb = ((MsgRect)objc_msgSend)(obs, sel_getUid("boundingBox"));

        r = &page->runs[page->count];
        r->text = (char *)malloc(strlen(utf8) + 1);
        if (!r->text) continue;
        strcpy(r->text, utf8);

        r->page = 1;
        /*
         * Normalised (0..1, origin bottom-left) to PIXELS OF THE SOURCE,
         * origin bottom-left.  The origin already matches std.pdf, so only
         * the unit is scaled; a consumer that sorts $textruns into reading
         * order sorts these with the identical comparison.
         */
        r->x          = bb.origin.x    * iw;
        r->y          = bb.origin.y    * ih;
        r->width      = bb.size.width  * iw;
        r->height     = bb.size.height * ih;
        r->confidence = (double)((MsgFlt)objc_msgSend)(cand, sel_getUid("confidence"));

        page->count++;
    }

    ms0(req, "release");
    ms0(handler, "release");
    clear_err();
    return page;
}

TkOcrPage *ocr_recognize_mem(const uint8_t *bytes, uint64_t len)
{
    CGImageRef img;
    TkOcrPage *page;

    clear_err();
    if (!ocr_is_available()) {
        set_err(OCR_ERR_NOENGINE, "no text recogniser is available on this host");
        return NULL;
    }
    img  = image_from_bytes(bytes, len);
    page = recognize_cgimage(img);
    if (img) CGImageRelease(img);
    return page;
}

TkOcrPage *ocr_recognize_file(const char *path)
{
    CFStringRef s;
    CFURLRef url;
    CGImageSourceRef src;
    CGImageRef img;
    TkOcrPage *page;

    clear_err();
    if (!path || !*path) {
        set_err(OCR_ERR_IO, "empty path");
        return NULL;
    }
    if (!ocr_is_available()) {
        set_err(OCR_ERR_NOENGINE, "no text recogniser is available on this host");
        return NULL;
    }

    s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    if (!s) { set_err(OCR_ERR_IO, "path is not valid UTF-8"); return NULL; }
    url = CFURLCreateWithFileSystemPath(NULL, s, kCFURLPOSIXPathStyle, false);
    CFRelease(s);
    if (!url) { set_err(OCR_ERR_IO, "path could not be resolved"); return NULL; }

    src = CGImageSourceCreateWithURL(url, NULL);
    CFRelease(url);
    if (!src) {
        set_err(OCR_ERR_IO, "the file could not be read as an image");
        return NULL;
    }
    img = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);

    page = recognize_cgimage(img);
    if (img) CGImageRelease(img);
    return page;
}

TkOcrPage *ocr_recognize_raw(const uint8_t *pixels, uint32_t width,
                             uint32_t height, uint32_t channels)
{
    CGImageRef img;
    TkOcrPage *page;

    clear_err();
    if (!ocr_is_available()) {
        set_err(OCR_ERR_NOENGINE, "no text recogniser is available on this host");
        return NULL;
    }
    if (channels != 1 && channels != 3 && channels != 4) {
        set_err(OCR_ERR_BADIMAGE, "channels must be 1, 3 or 4");
        return NULL;
    }
    img  = image_from_raw(pixels, width, height, channels);
    page = recognize_cgimage(img);
    if (img) CGImageRelease(img);
    return page;
}

/* ══════════════════════════════════════════════════════════════════════
 * Every other platform — NOENGINE, never an empty success
 *
 * Windows is included here on purpose.  The story row names the Windows
 * OCR API; this machine cannot test it, and an untested branch that
 * returns plausible values is the defect class check-glue-core exists to
 * catch.  A Windows caller learns there is no recogniser, which is true,
 * rather than that the page is blank, which is not.
 * ══════════════════════════════════════════════════════════════════════ */
#else

static const char *no_engine_msg(void)
{
    return "no system text recogniser on this platform "
           "(macOS Vision only; see docs/stdlib/ocr.md)";
}

int         ocr_is_available(void) { return 0; }
const char *ocr_engine(void)       { return "none"; }
const char *ocr_languages(void)    { return ""; }

int ocr_set_languages(const char *langs)
{
    (void)langs;
    set_err(OCR_ERR_NOENGINE, no_engine_msg());
    return 0;
}

void ocr_set_fast(int fast) { (void)fast; }

TkOcrPage *ocr_recognize_mem(const uint8_t *bytes, uint64_t len)
{
    (void)bytes; (void)len;
    set_err(OCR_ERR_NOENGINE, no_engine_msg());
    return NULL;
}

TkOcrPage *ocr_recognize_file(const char *path)
{
    (void)path;
    set_err(OCR_ERR_NOENGINE, no_engine_msg());
    return NULL;
}

TkOcrPage *ocr_recognize_raw(const uint8_t *pixels, uint32_t width,
                             uint32_t height, uint32_t channels)
{
    (void)pixels; (void)width; (void)height; (void)channels;
    set_err(OCR_ERR_NOENGINE, no_engine_msg());
    return NULL;
}

#endif /* __APPLE__ */
