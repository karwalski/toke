/*
 * pdffont.h — the generated standard-14 metric and encoding tables.
 *
 * Populated by scripts/gen_pdf_fontdata.py into pdffont.c.  See the comment
 * at the top of that file for where every number comes from.
 *
 * Story: 135.3
 */

#ifndef TK_STDLIB_PDFFONT_H
#define TK_STDLIB_PDFFONT_H

/* Index into TkPdfGlyph.w — the six distinct standard-14 width vectors. */
#define TK_PDF_WV_HELV      0
#define TK_PDF_WV_HELV_B    1
#define TK_PDF_WV_TIMES     2
#define TK_PDF_WV_TIMES_B   3
#define TK_PDF_WV_TIMES_I   4
#define TK_PDF_WV_TIMES_BI  5
#define TK_PDF_WV_COURIER  -1   /* 600 for every glyph; not tabulated */
#define TK_PDF_WV_NONE     -2   /* not a standard-14 font */

typedef struct {
    const char    *name;   /* PostScript glyph name                        */
    unsigned short uni;    /* Unicode code point, 0 if none                */
    unsigned short w[6];   /* advance in 1/1000 em, 0 if absent from font  */
} TkPdfGlyph;

extern const TkPdfGlyph tk_pdf_glyphs[];
extern const unsigned   tk_pdf_glyph_count;

/* code -> index into tk_pdf_glyphs, -1 where the encoding leaves it unused */
extern const short tk_pdf_enc_std[256];
extern const short tk_pdf_enc_win[256];
extern const short tk_pdf_enc_mac[256];

#endif /* TK_STDLIB_PDFFONT_H */
