/*
 * pdftext.c — the content-stream interpreter: text runs with positions.
 *
 * This is the part of std.pdf that story 135.3 is actually about.  A flat
 * string has thrown away the column structure before the caller sees it, and
 * a bank statement is a table; the positions are what make it readable.
 *
 * ── WHAT A RUN IS ───────────────────────────────────────────────────────
 *
 * One text-showing operator — one Tj, TJ, ' or ".  That is the producer's
 * own unit of text: a table cell is one drawString is one Tj.  Using it
 * means this module never has to GUESS where a column boundary lies, which
 * is the single most common way a PDF extractor invents structure that is
 * not in the file.  Merging adjacent runs would destroy the columns;
 * splitting finer would fabricate boundaries.
 *
 * ── THE MATRICES, WHICH ARE WHERE POSITIONS COME FROM ───────────────────
 *
 * A glyph's position is not in the content stream.  It is the product of
 * three transforms, and a reader that keeps only one of them reports
 * positions that are right for simple documents and wrong for every
 * document that scales, translates or nests:
 *
 *     Trm = [Tfs*Th  0      ]   x  Tm  x  CTM
 *           [0       Tfs    ]
 *           [0       Trise  ]
 *
 * CTM comes from cm and from the /Matrix of every form XObject we are
 * inside; Tm from Tm/Td/TD/T*; the leftmost from Tf, Tz and Ts.  All three
 * are tracked here.  `fontsize` in a run is therefore the EFFECTIVE size —
 * a document that sets a 1pt font and scales it 12x in Tm is showing 12pt
 * text, and reporting 1 would be a plausible wrong answer.
 *
 * ── THE ADVANCE, WHICH IS WHERE WIDTHS COME FROM ────────────────────────
 *
 *     tx = ((w0/1000) * Tfs + Tc + Tw) * Th
 *
 * w0 is the glyph's own advance width, from /Widths, /W, or the compiled
 * standard-14 metrics (pdffont.c).  Tw applies to the single byte 32 only,
 * never to a 2-byte CID code — applying it to both halves of every CID is a
 * classic off-by-a-word.
 *
 * Story: 135.3
 */

#include "pdf.h"
#include "pdfobj.h"
#include "pdffont.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── Matrices ────────────────────────────────────────────────────────── */

typedef struct { double a, b, c, d, e, f; } Mat;

static const Mat MAT_ID = { 1, 0, 0, 1, 0, 0 };

/* m x n, in PDF's row-vector convention. */
static Mat mat_mul(Mat m, Mat n)
{
    Mat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

/* ── Fonts ───────────────────────────────────────────────────────────── */

typedef struct { uint32_t code; char utf8[12]; } CMapEnt;
typedef struct { uint32_t lo, hi; double w; } CidW;

typedef struct {
    PdfObj *dict;             /* identity for the cache                  */
    int     twobyte;          /* composite font with 2-byte codes        */
    int     wv;               /* TK_PDF_WV_* index into tk_pdf_glyphs.w  */

    int     firstchar;
    double  widths[256];      /* 1/1000 em; < 0 means "not given"        */
    int     haswidths;
    double  missingwidth;

    double  defwidth;         /* /DW for a composite font                */
    CidW   *cidw;
    uint32_t ncidw;

    const short *baseenc;     /* tk_pdf_enc_*; NULL for a composite font */
    short   diff[256];        /* /Differences: glyph index per code, -1  */
    int     hasdiff;

    CMapEnt *cmap;            /* /ToUnicode, sorted by code              */
    uint32_t ncmap, capcmap;

    double  ascent, descent;  /* 1/1000 em                               */
} Font;

typedef struct {
    TkPdfDoc  *doc;
    uint32_t   pageno;
    TkPdfRun **runs;
    uint32_t  *nruns, *cap;
    Font     **fonts;
    uint32_t   nfonts, capfonts;
} Ctx;

/* ── UTF-8 ───────────────────────────────────────────────────────────── */

static size_t utf8_put(char *out, size_t room, uint32_t cp)
{
    if (cp < 0x80)        { if (room < 1) return 0; out[0] = (char)cp; return 1; }
    if (cp < 0x800)       { if (room < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000)     { if (room < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    if (room < 4) return 0;
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* UTF-16BE, as every /ToUnicode target is, into UTF-8.  Surrogate pairs are
 * joined: a mapping to an astral character arrives as two code units and a
 * reader that emits them separately produces two replacement characters. */
static void utf16be_to_utf8(const char *src, size_t n, char *out, size_t room)
{
    size_t k = 0;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = (uint32_t)((unsigned char)src[i] << 8 | (unsigned char)src[i + 1]);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
            uint32_t lo = (uint32_t)((unsigned char)src[i + 2] << 8 | (unsigned char)src[i + 3]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (k + 5 > room) break;
        k += utf8_put(out + k, room - k, u);
    }
    out[k < room ? k : room - 1] = '\0';
}

/* ── Glyph table lookup ──────────────────────────────────────────────── */

static int glyph_index(const char *name)
{
    unsigned lo = 0, hi = tk_pdf_glyph_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(name, tk_pdf_glyphs[mid].name);
        if (c == 0) return (int)mid;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return -1;
}

/*
 * The standard-14 width vector a base font name selects.
 *
 * A subset-embedded font carries a `ABCDEF+` prefix; it is stripped because
 * the prefix is arbitrary and matching on the full name would send every
 * subset of Helvetica down the "unknown font" path.
 */
static int width_vector_for(const char *base)
{
    const char *p;
    if (!base) return TK_PDF_WV_NONE;
    p = strchr(base, '+');
    if (p && (size_t)(p - base) == 6) base = p + 1;

    if (!strncmp(base, "Courier", 7) || !strncmp(base, "Mono", 4)) return TK_PDF_WV_COURIER;
    if (!strncmp(base, "Helvetica", 9) || !strncmp(base, "Arial", 5)) {
        return strstr(base, "Bold") ? TK_PDF_WV_HELV_B : TK_PDF_WV_HELV;
    }
    if (!strncmp(base, "Times", 5)) {
        int bold = strstr(base, "Bold") != NULL;
        int ital = strstr(base, "Italic") != NULL;
        if (bold && ital) return TK_PDF_WV_TIMES_BI;
        if (bold) return TK_PDF_WV_TIMES_B;
        if (ital) return TK_PDF_WV_TIMES_I;
        return TK_PDF_WV_TIMES;
    }
    return TK_PDF_WV_NONE;
}

/* ── /ToUnicode ──────────────────────────────────────────────────────── */

static int cmap_cmp(const void *a, const void *b)
{
    uint32_t x = ((const CMapEnt *)a)->code, y = ((const CMapEnt *)b)->code;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int cmap_push(Font *f, uint32_t code, const char *utf16, size_t n)
{
    if (f->ncmap >= TK_PDF_MAX_CMAP) return -1;
    if (f->ncmap == f->capcmap) {
        uint32_t nc = f->capcmap ? f->capcmap * 2 : 64;
        CMapEnt *ne = (CMapEnt *)realloc(f->cmap, nc * sizeof(CMapEnt));
        if (!ne) return -1;
        f->cmap = ne;
        f->capcmap = nc;
    }
    f->cmap[f->ncmap].code = code;
    utf16be_to_utf8(utf16, n, f->cmap[f->ncmap].utf8, sizeof f->cmap[0].utf8);
    f->ncmap++;
    return 0;
}

static uint32_t hexcode(const char *s, size_t n)
{
    uint32_t v = 0;
    for (size_t i = 0; i < n && i < 4; i++) v = (v << 8) | (uint32_t)(unsigned char)s[i];
    return v;
}

/*
 * parse_tounicode — the bfchar/bfrange sections of a /ToUnicode CMap.
 *
 * This is the ONLY correct source of meaning for an embedded subset font,
 * and it is also where ligatures live: `<0001> <00660069>` says code 1 is
 * the TWO characters "fi".  A reader that assumes one code is one character
 * drops the second half and produces "f" where the page shows "fi" — which
 * is a word that still looks like a word.
 */
static void parse_tounicode(Ctx *cx, Font *f, PdfObj *fontdict)
{
    PdfObj *tu = pdf_dget(cx->doc, fontdict, "ToUnicode");
    const uint8_t *data;
    size_t dlen = 0;
    PdfLex L;
    char tok[64];

    if (!tu || tu->kind != PDF_STREAM) return;
    data = pdf_stream_data(cx->doc, tu, &dlen);
    if (!data) return;

    L.p = data; L.n = dlen; L.i = 0;
    while (L.i < L.n) {
        if (pdf_starts_object(&L)) { (void)pdf_parse(cx->doc, &L, 0); continue; }
        if (!pdf_token(&L, tok, sizeof tok)) break;

        if (!strcmp(tok, "beginbfchar")) {
            for (;;) {
                PdfObj *src, *dst;
                pdf_skipws(&L);
                if (!pdf_starts_object(&L)) { pdf_token(&L, tok, sizeof tok); break; }
                src = pdf_parse(cx->doc, &L, 0);
                dst = pdf_parse(cx->doc, &L, 0);
                if (!src || !dst || src->kind != PDF_STR || dst->kind != PDF_STR) break;
                if (cmap_push(f, hexcode(src->u.str.p, src->u.str.n),
                              dst->u.str.p, dst->u.str.n) != 0) return;
            }
        } else if (!strcmp(tok, "beginbfrange")) {
            for (;;) {
                PdfObj *lo, *hi, *dst;
                pdf_skipws(&L);
                if (!pdf_starts_object(&L)) { pdf_token(&L, tok, sizeof tok); break; }
                lo  = pdf_parse(cx->doc, &L, 0);
                hi  = pdf_parse(cx->doc, &L, 0);
                dst = pdf_parse(cx->doc, &L, 0);
                if (!lo || !hi || !dst || lo->kind != PDF_STR || hi->kind != PDF_STR) break;
                {
                    uint32_t a = hexcode(lo->u.str.p, lo->u.str.n);
                    uint32_t b = hexcode(hi->u.str.p, hi->u.str.n);
                    if (b < a || b - a > TK_PDF_MAX_CMAP) break;
                    if (dst->kind == PDF_ARR) {
                        for (uint32_t i = 0; a + i <= b && i < pdf_arr_len(dst); i++) {
                            PdfObj *e = pdf_arr_get(cx->doc, dst, i);
                            if (e && e->kind == PDF_STR &&
                                cmap_push(f, a + i, e->u.str.p, e->u.str.n) != 0) return;
                        }
                    } else if (dst->kind == PDF_STR && dst->u.str.n >= 2) {
                        /* The destination increments in its LAST code unit. */
                        char buf[16];
                        size_t n = dst->u.str.n < sizeof buf ? dst->u.str.n : sizeof buf;
                        for (uint32_t c = a; c <= b; c++) {
                            uint32_t bump = c - a;
                            memcpy(buf, dst->u.str.p, n);
                            {
                                uint32_t last = (uint32_t)((unsigned char)buf[n - 2] << 8 |
                                                           (unsigned char)buf[n - 1]) + bump;
                                buf[n - 2] = (char)((last >> 8) & 0xFF);
                                buf[n - 1] = (char)(last & 0xFF);
                            }
                            if (cmap_push(f, c, buf, n) != 0) return;
                        }
                    }
                }
            }
        }
    }
    if (f->ncmap) qsort(f->cmap, f->ncmap, sizeof(CMapEnt), cmap_cmp);
}

static const char *cmap_lookup(const Font *f, uint32_t code)
{
    unsigned lo = 0, hi = f->ncmap;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (f->cmap[mid].code == code) return f->cmap[mid].utf8;
        if (code < f->cmap[mid].code) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* ── Font loading ────────────────────────────────────────────────────── */

static void load_encoding(Ctx *cx, Font *f, PdfObj *fontdict, int symbolic)
{
    PdfObj *enc = pdf_dget(cx->doc, fontdict, "Encoding");
    const char *base = pdf_name_of(enc);

    /* A non-symbolic simple font with no /Encoding uses the font's built-in
     * encoding, which for every standard-14 text font is StandardEncoding.
     * A symbolic font's built-in encoding is its own business and no base
     * encoding is a better guess than none. */
    f->baseenc = symbolic ? NULL : tk_pdf_enc_std;

    if (!base && enc && enc->kind == PDF_DICT)
        base = pdf_name_of(pdf_dget(cx->doc, enc, "BaseEncoding"));
    if (base) {
        if (!strcmp(base, "WinAnsiEncoding"))        f->baseenc = tk_pdf_enc_win;
        else if (!strcmp(base, "MacRomanEncoding"))  f->baseenc = tk_pdf_enc_mac;
        else if (!strcmp(base, "StandardEncoding"))  f->baseenc = tk_pdf_enc_std;
    }

    for (int i = 0; i < 256; i++) f->diff[i] = -1;
    if (enc && enc->kind == PDF_DICT) {
        PdfObj *d = pdf_dget(cx->doc, enc, "Differences");
        int code = 0;
        for (uint32_t i = 0; i < pdf_arr_len(d); i++) {
            PdfObj *e = pdf_arr_get(cx->doc, d, i);
            if (!e) continue;
            if (e->kind == PDF_NUM) { code = (int)e->u.num; continue; }
            if (e->kind == PDF_NAME && code >= 0 && code < 256) {
                f->diff[code] = (short)glyph_index(e->u.name);
                f->hasdiff = 1;
                code++;
            }
        }
    }
}

static void load_cid_widths(Ctx *cx, Font *f, PdfObj *desc)
{
    PdfObj *W = pdf_dget(cx->doc, desc, "W");
    uint32_t n = pdf_arr_len(W), i = 0, cap = 0;
    f->defwidth = pdf_num(pdf_dget(cx->doc, desc, "DW"), 1000.0);
    while (i < n) {
        PdfObj *a = pdf_arr_get(cx->doc, W, i);
        PdfObj *b = pdf_arr_get(cx->doc, W, i + 1);
        if (!a || a->kind != PDF_NUM || !b) break;
        if (f->ncidw >= cap) {
            uint32_t nc = cap ? cap * 2 : 32;
            CidW *nw = (CidW *)realloc(f->cidw, nc * sizeof(CidW));
            if (!nw) return;
            f->cidw = nw;
            cap = nc;
        }
        if (b->kind == PDF_ARR) {              /* c [w1 w2 ...] */
            uint32_t m = pdf_arr_len(b);
            for (uint32_t j = 0; j < m && f->ncidw < TK_PDF_MAX_WIDTHS; j++) {
                PdfObj *w = pdf_arr_get(cx->doc, b, j);
                if (f->ncidw >= cap) {
                    uint32_t nc = cap * 2;
                    CidW *nw = (CidW *)realloc(f->cidw, nc * sizeof(CidW));
                    if (!nw) return;
                    f->cidw = nw;
                    cap = nc;
                }
                f->cidw[f->ncidw].lo = f->cidw[f->ncidw].hi = (uint32_t)a->u.num + j;
                f->cidw[f->ncidw].w = pdf_num(w, f->defwidth);
                f->ncidw++;
            }
            i += 2;
        } else {                               /* cfirst clast w */
            PdfObj *w = pdf_arr_get(cx->doc, W, i + 2);
            if (b->kind != PDF_NUM || !w) break;
            f->cidw[f->ncidw].lo = (uint32_t)a->u.num;
            f->cidw[f->ncidw].hi = (uint32_t)b->u.num;
            f->cidw[f->ncidw].w  = pdf_num(w, f->defwidth);
            f->ncidw++;
            i += 3;
        }
        if (f->ncidw >= TK_PDF_MAX_WIDTHS) break;
    }
}

static void load_descriptor(Ctx *cx, Font *f, PdfObj *holder)
{
    PdfObj *fd = pdf_dget(cx->doc, holder, "FontDescriptor");
    if (!fd) return;
    f->ascent  = pdf_num(pdf_dget(cx->doc, fd, "Ascent"),  f->ascent);
    f->descent = pdf_num(pdf_dget(cx->doc, fd, "Descent"), f->descent);
    f->missingwidth = pdf_num(pdf_dget(cx->doc, fd, "MissingWidth"), f->missingwidth);
}

static Font *font_load(Ctx *cx, PdfObj *fontdict)
{
    Font *f;
    PdfObj *sub;
    const char *subtype;
    int symbolic = 0;

    for (uint32_t i = 0; i < cx->nfonts; i++)
        if (cx->fonts[i]->dict == fontdict) return cx->fonts[i];
    if (cx->nfonts >= TK_PDF_MAX_FONTS) return NULL;

    f = (Font *)calloc(1, sizeof(Font));
    if (!f) return NULL;
    f->dict = fontdict;
    f->ascent = 750;
    f->descent = -250;
    f->missingwidth = 0;
    f->defwidth = 1000;
    for (int i = 0; i < 256; i++) f->widths[i] = -1;

    sub = pdf_dget(cx->doc, fontdict, "Subtype");
    subtype = pdf_name_of(sub);
    f->wv = width_vector_for(pdf_name_of(pdf_dget(cx->doc, fontdict, "BaseFont")));

    {   /* the symbolic flag decides whether a base encoding may be assumed */
        PdfObj *fd = pdf_dget(cx->doc, fontdict, "FontDescriptor");
        int flags = (int)pdf_num(pdf_dget(cx->doc, fd, "Flags"), 0);
        symbolic = (flags & 4) && !(flags & 32);
    }

    if (subtype && !strcmp(subtype, "Type0")) {
        /*
         * A composite font.  Only Identity-H/V and the predefined CMaps'
         * common 2-byte case are handled: the code is two bytes and means a
         * CID.  The meaning of that CID comes from /ToUnicode and from
         * nowhere else — which is exactly the case pdf.h's trap 3 is about.
         */
        PdfObj *descf = pdf_arr_get(cx->doc, pdf_dget(cx->doc, fontdict, "DescendantFonts"), 0);
        f->twobyte = 1;
        f->baseenc = NULL;
        if (descf) {
            load_cid_widths(cx, f, descf);
            load_descriptor(cx, f, descf);
        }
    } else {
        PdfObj *w = pdf_dget(cx->doc, fontdict, "Widths");
        f->firstchar = (int)pdf_num(pdf_dget(cx->doc, fontdict, "FirstChar"), 0);
        load_descriptor(cx, f, fontdict);
        load_encoding(cx, f, fontdict, symbolic);
        if (pdf_arr_len(w) > 0 && pdf_arr_len(w) <= TK_PDF_MAX_WIDTHS) {
            uint32_t n = pdf_arr_len(w);
            double scale = 1.0;
            if (subtype && !strcmp(subtype, "Type3")) {
                /* Type3 widths are in glyph space; /FontMatrix maps them. */
                PdfObj *fm = pdf_dget(cx->doc, fontdict, "FontMatrix");
                if (pdf_arr_len(fm) >= 1)
                    scale = pdf_num(pdf_arr_get(cx->doc, fm, 0), 0.001) * 1000.0;
            }
            for (uint32_t i = 0; i < n; i++) {
                int code = f->firstchar + (int)i;
                if (code < 0 || code > 255) continue;
                f->widths[code] = pdf_num(pdf_arr_get(cx->doc, w, i), -1) * scale;
            }
            f->haswidths = 1;
        }
    }

    parse_tounicode(cx, f, fontdict);

    if (cx->nfonts == cx->capfonts) {
        uint32_t nc = cx->capfonts ? cx->capfonts * 2 : 8;
        Font **nf = (Font **)realloc(cx->fonts, nc * sizeof(Font *));
        if (!nf) { free(f->cmap); free(f->cidw); free(f); return NULL; }
        cx->fonts = nf;
        cx->capfonts = nc;
    }
    cx->fonts[cx->nfonts++] = f;
    return f;
}

/* ── Per-code width and meaning ──────────────────────────────────────── */

/* Advance width of `code`, in 1/1000 em. */
static double code_width(const Font *f, uint32_t code)
{
    if (!f) return 500;
    if (f->twobyte) {
        for (uint32_t i = 0; i < f->ncidw; i++)
            if (code >= f->cidw[i].lo && code <= f->cidw[i].hi) return f->cidw[i].w;
        return f->defwidth;
    }
    if (f->haswidths && code < 256 && f->widths[code] >= 0) return f->widths[code];
    /* No /Widths: the standard-14 metrics, which is the case producers rely
     * on and the reason pdffont.c exists. */
    if (f->wv == TK_PDF_WV_COURIER) return 600;
    if (f->wv >= 0 && code < 256) {
        int gi = f->hasdiff && f->diff[code] >= 0 ? f->diff[code]
               : (f->baseenc ? f->baseenc[code] : -1);
        if (gi < 0 && !f->baseenc) gi = tk_pdf_enc_std[code];
        if (gi >= 0) {
            unsigned short w = tk_pdf_glyphs[gi].w[f->wv];
            if (w) return (double)w;
        }
    }
    if (f->missingwidth > 0) return f->missingwidth;
    return 500;
}

/*
 * code_text — what `code` MEANS, appended as UTF-8.
 *
 * The order is the one the spec mandates and it matters: /ToUnicode
 * overrides everything, because it is the producer's own statement of what
 * its codes mean; then /Differences; then the base encoding.
 *
 * When nothing maps, U+FFFD is emitted rather than the raw byte.  That is
 * deliberate and is the defence named in pdf.h: a replacement character is
 * VISIBLY wrong, where a raw byte reinterpreted as Latin-1 is plausibly
 * wrong, and plausible wrong output is the failure mode this module is most
 * at risk of.  The one exception is a single-byte font in the ASCII range
 * with no encoding information at all, where the byte IS the character far
 * more often than not.
 */
static size_t code_text(const Font *f, uint32_t code, char *out, size_t room)
{
    const char *u;
    int gi = -1;

    if (f && f->ncmap && (u = cmap_lookup(f, code)) != NULL) {
        size_t n = strlen(u);
        if (n >= room) n = room ? room - 1 : 0;
        memcpy(out, u, n);
        return n;
    }
    if (f && !f->twobyte && code < 256) {
        if (f->hasdiff && f->diff[code] >= 0) gi = f->diff[code];
        else if (f->baseenc) gi = f->baseenc[code];
        if (gi >= 0 && tk_pdf_glyphs[gi].uni)
            return utf8_put(out, room, tk_pdf_glyphs[gi].uni);
        if (code >= 0x20 && code < 0x7F) { if (room) { out[0] = (char)code; return 1; } }
    }
    return utf8_put(out, room, 0xFFFD);
}

/* ── Graphics and text state ─────────────────────────────────────────── */

typedef struct {
    Mat    ctm;
    Font  *font;
    double fs, tc, tw, th, ts, tl;
} GState;

typedef struct {
    GState  gs[32];
    int     sp;
    Mat     tm, tlm;
    int     intext;
} State;

static int run_push(Ctx *cx, const char *text, size_t len, double x, double y,
                    double w, double h, double fs)
{
    TkPdfRun *r;
    char *copy;
    if (*cx->nruns >= TK_PDF_MAX_RUNS) {
        pdf_fail(TK_PDF_E_TOOLARGE, "pdf: more than %u text runs on one page",
                 TK_PDF_MAX_RUNS);
        return -1;
    }
    if (*cx->nruns == *cx->cap) {
        uint32_t nc = *cx->cap ? *cx->cap * 2 : 64;
        TkPdfRun *nr = (TkPdfRun *)realloc(*cx->runs, nc * sizeof(TkPdfRun));
        if (!nr) return -1;
        *cx->runs = nr;
        *cx->cap = nc;
    }
    copy = (char *)malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, text, len);
    copy[len] = '\0';

    r = &(*cx->runs)[(*cx->nruns)++];
    r->text = copy;
    r->page = (int64_t)cx->pageno;
    r->x = x; r->y = y; r->width = w; r->height = h; r->fontsize = fs;
    return 0;
}

/*
 * show_text — one text-showing operator becomes one run.
 *
 * `items` is the TJ array, or a single string for Tj.  The run's start point
 * is captured BEFORE any glyph advances, and its width is the distance the
 * pen travelled — measured in device space, so a rotated or scaled text
 * matrix gives a real distance rather than a text-space number that means
 * nothing on the page.
 */
static int show_text(Ctx *cx, State *st, PdfObj *items)
{
    GState *g = &st->gs[st->sp];
    Mat trm;
    char buf[TK_PDF_MAX_RUNBYTES];
    size_t blen = 0;
    double adv = 0;              /* total advance, in text space units    */
    double x0, y0, ux, uy, unit;
    uint32_t n, i;

    if (!g->font && !items) return 0;

    trm = mat_mul(st->tm, g->ctm);
    /* The glyph origin sits at (0, Ts) in text space. */
    x0 = trm.c * g->ts + trm.e;
    y0 = trm.d * g->ts + trm.f;
    ux = trm.a; uy = trm.b;                     /* baseline direction     */
    unit = sqrt(ux * ux + uy * uy);

    n = (items && items->kind == PDF_ARR) ? items->u.arr.n : 1;
    for (i = 0; i < n; i++) {
        PdfObj *e = (items && items->kind == PDF_ARR) ? pdf_arr_get(cx->doc, items, i) : items;
        const unsigned char *s;
        size_t slen;

        if (!e) continue;
        if (e->kind == PDF_NUM) {
            /* A positive number moves the pen LEFT (tighter); a negative one
             * moves it right, which is how many producers write a space
             * without writing a space character.  A gap wider than a quarter
             * em is one. */
            double tx = -e->u.num / 1000.0 * g->fs * g->th;
            adv += tx;
            if (tx > g->fs * 0.25 * g->th && blen && buf[blen - 1] != ' ' &&
                blen + 1 < sizeof buf)
                buf[blen++] = ' ';
            continue;
        }
        if (e->kind != PDF_STR) continue;

        s = (const unsigned char *)e->u.str.p;
        slen = e->u.str.n;
        for (size_t j = 0; j < slen; ) {
            uint32_t code;
            double w0, tx;
            if (g->font && g->font->twobyte) {
                if (j + 1 >= slen) break;
                code = (uint32_t)(s[j] << 8 | s[j + 1]);
                j += 2;
            } else {
                code = s[j++];
            }
            w0 = code_width(g->font, code);
            /* Tw applies to the single byte 32 and never to a CID. */
            tx = (w0 / 1000.0 * g->fs + g->tc +
                  ((!(g->font && g->font->twobyte) && code == 32) ? g->tw : 0)) * g->th;
            adv += tx;
            if (blen + 8 < sizeof buf)
                blen += code_text(g->font, code, buf + blen, sizeof buf - blen - 1);
        }
    }

    /* Advance the text matrix by the total, in text space. */
    {
        Mat tr = MAT_ID;
        tr.e = adv;
        st->tm = mat_mul(tr, st->tm);
    }

    if (blen) {
        double fs_eff = g->fs * sqrt(trm.c * trm.c + trm.d * trm.d);
        double em = g->font ? (g->font->ascent - g->font->descent) / 1000.0 : 1.0;
        if (em <= 0) em = 1.0;
        buf[blen] = '\0';
        return run_push(cx, buf, blen, x0, y0, adv * unit, em * fs_eff, fs_eff);
    }
    return 0;
}

/* ── Inline images ───────────────────────────────────────────────────── */

/*
 * skip_inline_image — step over BI ... ID <binary> EI.
 *
 * The bytes between ID and EI are raw image data and are NOT content-stream
 * syntax.  A lexer that keeps tokenising through them will read `(` and `<`
 * out of pixel data and lose synchronisation for the rest of the page — so
 * a page with one inline image loses all the text after it, silently.
 */
static void skip_inline_image(PdfLex *L)
{
    while (L->i + 1 < L->n) {
        if (L->p[L->i] == 'I' && L->p[L->i + 1] == 'D') { L->i += 2; break; }
        L->i++;
    }
    if (L->i < L->n) L->i++;                   /* the single whitespace byte */
    while (L->i + 1 < L->n) {
        if (L->p[L->i] == 'E' && L->p[L->i + 1] == 'I' &&
            (L->i == 0 || L->p[L->i - 1] == ' ' || L->p[L->i - 1] == '\n' ||
             L->p[L->i - 1] == '\r' || L->p[L->i - 1] == '\t') &&
            (L->i + 2 >= L->n || L->p[L->i + 2] == ' ' || L->p[L->i + 2] == '\n' ||
             L->p[L->i + 2] == '\r' || L->p[L->i + 2] == '\t')) {
            L->i += 2;
            return;
        }
        L->i++;
    }
    L->i = L->n;
}

/* ── The interpreter ─────────────────────────────────────────────────── */

static int run_content(Ctx *cx, PdfObj *resources, const uint8_t *content,
                       size_t len, State *st, unsigned formdepth);

/* Do — a form XObject is a content stream in its own right.  Its /Matrix
 * premultiplies the CTM and its /Resources replace the page's.  A reader
 * that ignores forms loses every piece of text a producer factored out,
 * which for letterheads and repeated table headers is most of the page. */
static int do_xobject(Ctx *cx, PdfObj *resources, State *st, const char *name,
                      unsigned formdepth)
{
    PdfObj *xd = pdf_dget(cx->doc, pdf_dget(cx->doc, resources, "XObject"), name);
    PdfObj *mtx, *res;
    const uint8_t *data;
    size_t dlen = 0;
    State sub;

    if (!xd || xd->kind != PDF_STREAM) return 0;
    if (!pdf_is_name(pdf_dget(cx->doc, xd, "Subtype"), "Form")) return 0;
    if (formdepth >= TK_PDF_MAX_FORMDEPTH) return 0;

    data = pdf_stream_data(cx->doc, xd, &dlen);
    if (!data) return 0;

    sub = *st;
    sub.sp = 0;
    sub.gs[0] = st->gs[st->sp];
    sub.intext = 0;
    mtx = pdf_dget(cx->doc, xd, "Matrix");
    if (pdf_arr_len(mtx) >= 6) {
        Mat m;
        m.a = pdf_num(pdf_arr_get(cx->doc, mtx, 0), 1);
        m.b = pdf_num(pdf_arr_get(cx->doc, mtx, 1), 0);
        m.c = pdf_num(pdf_arr_get(cx->doc, mtx, 2), 0);
        m.d = pdf_num(pdf_arr_get(cx->doc, mtx, 3), 1);
        m.e = pdf_num(pdf_arr_get(cx->doc, mtx, 4), 0);
        m.f = pdf_num(pdf_arr_get(cx->doc, mtx, 5), 0);
        sub.gs[0].ctm = mat_mul(m, sub.gs[0].ctm);
    }
    res = pdf_dget(cx->doc, xd, "Resources");
    return run_content(cx, res ? res : resources, data, dlen, &sub, formdepth + 1);
}

static int run_content(Ctx *cx, PdfObj *resources, const uint8_t *content,
                       size_t len, State *st, unsigned formdepth)
{
    PdfLex L;
    PdfObj *stack[8];
    int nstack = 0;
    char op[64];

    L.p = content; L.n = len; L.i = 0;

    for (;;) {
        pdf_skipws(&L);
        if (L.i >= L.n) break;

        if (pdf_starts_object(&L)) {
            PdfObj *o = pdf_parse(cx->doc, &L, 0);
            if (!o) { if (!pdf_token(&L, op, sizeof op)) break; continue; }
            if (nstack < (int)(sizeof stack / sizeof stack[0])) stack[nstack++] = o;
            continue;
        }
        if (!pdf_token(&L, op, sizeof op)) break;
        if (!op[0]) break;

        {
            GState *g = &st->gs[st->sp];
            PdfObj *a0 = nstack > 0 ? stack[nstack - 1] : NULL;

            if (!strcmp(op, "q")) {
                if (st->sp + 1 < (int)(sizeof st->gs / sizeof st->gs[0])) {
                    st->gs[st->sp + 1] = *g;
                    st->sp++;
                }
            } else if (!strcmp(op, "Q")) {
                if (st->sp > 0) st->sp--;
            } else if (!strcmp(op, "cm") && nstack >= 6) {
                Mat m;
                m.a = pdf_num(stack[nstack - 6], 1); m.b = pdf_num(stack[nstack - 5], 0);
                m.c = pdf_num(stack[nstack - 4], 0); m.d = pdf_num(stack[nstack - 3], 1);
                m.e = pdf_num(stack[nstack - 2], 0); m.f = pdf_num(stack[nstack - 1], 0);
                g->ctm = mat_mul(m, g->ctm);
            } else if (!strcmp(op, "BT")) {
                st->tm = st->tlm = MAT_ID;
                st->intext = 1;
            } else if (!strcmp(op, "ET")) {
                st->intext = 0;
            } else if (!strcmp(op, "Tf") && nstack >= 2) {
                const char *fname = pdf_name_of(stack[nstack - 2]);
                g->fs = pdf_num(a0, g->fs);
                if (fname) {
                    PdfObj *fd = pdf_dget(cx->doc, pdf_dget(cx->doc, resources, "Font"), fname);
                    if (fd) g->font = font_load(cx, fd);
                }
            } else if (!strcmp(op, "Td") && nstack >= 2) {
                Mat t = MAT_ID;
                t.e = pdf_num(stack[nstack - 2], 0);
                t.f = pdf_num(a0, 0);
                st->tlm = mat_mul(t, st->tlm);
                st->tm = st->tlm;
            } else if (!strcmp(op, "TD") && nstack >= 2) {
                Mat t = MAT_ID;
                t.e = pdf_num(stack[nstack - 2], 0);
                t.f = pdf_num(a0, 0);
                g->tl = -t.f;
                st->tlm = mat_mul(t, st->tlm);
                st->tm = st->tlm;
            } else if (!strcmp(op, "Tm") && nstack >= 6) {
                Mat m;
                m.a = pdf_num(stack[nstack - 6], 1); m.b = pdf_num(stack[nstack - 5], 0);
                m.c = pdf_num(stack[nstack - 4], 0); m.d = pdf_num(stack[nstack - 3], 1);
                m.e = pdf_num(stack[nstack - 2], 0); m.f = pdf_num(stack[nstack - 1], 0);
                st->tm = st->tlm = m;
            } else if (!strcmp(op, "T*")) {
                Mat t = MAT_ID;
                t.f = -g->tl;
                st->tlm = mat_mul(t, st->tlm);
                st->tm = st->tlm;
            } else if (!strcmp(op, "TL")) { g->tl = pdf_num(a0, g->tl);
            } else if (!strcmp(op, "Tc")) { g->tc = pdf_num(a0, g->tc);
            } else if (!strcmp(op, "Tw")) { g->tw = pdf_num(a0, g->tw);
            } else if (!strcmp(op, "Tz")) { g->th = pdf_num(a0, 100) / 100.0;
            } else if (!strcmp(op, "Ts")) { g->ts = pdf_num(a0, g->ts);
            } else if (!strcmp(op, "Tj") || !strcmp(op, "TJ")) {
                if (show_text(cx, st, a0) != 0) return -1;
            } else if (!strcmp(op, "'")) {
                Mat t = MAT_ID;
                t.f = -g->tl;
                st->tlm = mat_mul(t, st->tlm);
                st->tm = st->tlm;
                if (show_text(cx, st, a0) != 0) return -1;
            } else if (!strcmp(op, "\"") && nstack >= 3) {
                Mat t = MAT_ID;
                g->tw = pdf_num(stack[nstack - 3], g->tw);
                g->tc = pdf_num(stack[nstack - 2], g->tc);
                t.f = -g->tl;
                st->tlm = mat_mul(t, st->tlm);
                st->tm = st->tlm;
                if (show_text(cx, st, a0) != 0) return -1;
            } else if (!strcmp(op, "Do")) {
                const char *nm = pdf_name_of(a0);
                if (nm && do_xobject(cx, resources, st, nm, formdepth) != 0) return -1;
            } else if (!strcmp(op, "BI")) {
                skip_inline_image(&L);
            }
        }
        nstack = 0;
    }
    return 0;
}

int pdf_extract_runs(TkPdfDoc *doc, PdfObj *resources,
                     const uint8_t *content, size_t len,
                     uint32_t pageno, TkPdfRun **runs, uint32_t *nruns,
                     uint32_t *cap)
{
    Ctx cx;
    State st;
    int rc;

    memset(&cx, 0, sizeof cx);
    cx.doc = doc;
    cx.pageno = pageno;
    cx.runs = runs;
    cx.nruns = nruns;
    cx.cap = cap;

    memset(&st, 0, sizeof st);
    st.gs[0].ctm = MAT_ID;
    st.gs[0].th = 1.0;
    st.gs[0].fs = 0;
    st.tm = st.tlm = MAT_ID;

    rc = run_content(&cx, resources, content, len, &st, 0);

    for (uint32_t i = 0; i < cx.nfonts; i++) {
        free(cx.fonts[i]->cmap);
        free(cx.fonts[i]->cidw);
        free(cx.fonts[i]);
    }
    free(cx.fonts);
    return rc;
}
