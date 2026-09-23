/*
 * pdfobj.h — the PDF object model, shared between pdf.c and pdftext.c.
 *
 * INTERNAL to std.pdf.  Nothing outside src/stdlib/pdf*.c includes this;
 * the module's surface is pdf.h.
 *
 * MEMORY MODEL.  Every PdfObj belongs to the TkPdfDoc that parsed it and is
 * registered in a flat pool at birth, so releasing a document is one sweep
 * over that pool and no object is ever individually owned, refcounted or
 * freed.  That is the whole reason a PDF's object graph — which is a graph,
 * with cycles, shared subtrees and self-referential page trees — is safe to
 * walk here without a single ownership question.
 *
 * Story: 135.3
 */

#ifndef TK_STDLIB_PDFOBJ_H
#define TK_STDLIB_PDFOBJ_H

#include <stddef.h>
#include <stdint.h>

#include "pdf.h"   /* TkPdfDoc and TkPdfRun; this header only extends it */

typedef enum {
    PDF_NULL = 0,
    PDF_BOOL,
    PDF_NUM,
    PDF_STR,     /* a PDF string, already unescaped; may hold NUL bytes */
    PDF_NAME,    /* without the leading '/', with #xx already decoded   */
    PDF_ARR,
    PDF_DICT,
    PDF_REF,     /* "12 0 R" — resolve with pdf_resolve()              */
    PDF_STREAM   /* a dict plus raw (still encoded) bytes              */
} PdfKind;

typedef struct PdfObj PdfObj;

struct PdfObj {
    PdfKind kind;
    union {
        int    b;
        double num;
        struct { char    *p; size_t n; }               str;
        char  *name;
        struct { PdfObj **v; uint32_t n, cap; }        arr;
        struct { char **k; PdfObj **v; uint32_t n, cap; } dict;
        struct { int32_t num, gen; }                   ref;
        /* `dec`/`declen` memoise pdf_stream_data's result.  Without the
         * memo, every call re-inflates: `pdf.runs` then `pdf.pagetext` on
         * one page inflates it twice, and a loop calling either repeatedly
         * grows the document pool without bound — each round allocating up
         * to TK_PDF_MAX_STREAM.  The decoded buffer is pooled and lives as
         * long as the document anyway, so caching it costs nothing and
         * removes the growth. */
        struct { PdfObj *dict; const uint8_t *raw; size_t rawlen;
                 const uint8_t *dec; size_t declen; int decoded; } stream;
    } u;
};

/* ── Accessors.  Every one of these is NULL-safe and kind-safe: they answer
 *    "not that" rather than trapping, because a PDF is hostile input and
 *    every field in it is optional until proven otherwise. ─────────────── */

/* Resolve an indirect reference (repeatedly, with a cycle guard).  Any
 * other object is returned unchanged.  NULL in, NULL out. */
PdfObj *pdf_resolve(TkPdfDoc *doc, PdfObj *o);

/* Dictionary lookup by key, then resolve.  Works on a PDF_STREAM too, which
 * is what makes `/Length` and `/Filter` reachable without unwrapping. */
PdfObj *pdf_dget(TkPdfDoc *doc, PdfObj *d, const char *key);

/* Typed readers.  `def` is returned when the object is absent or the wrong
 * kind — the two cases a PDF makes indistinguishable anyway. */
double      pdf_num(PdfObj *o, double def);
int         pdf_is_name(PdfObj *o, const char *name);
const char *pdf_name_of(PdfObj *o);          /* NULL if not a name        */
uint32_t    pdf_arr_len(PdfObj *o);
PdfObj     *pdf_arr_get(TkPdfDoc *doc, PdfObj *o, uint32_t i);

/* Decode a stream's bytes through its /Filter chain.  Returns a buffer the
 * DOCUMENT owns (pooled, freed at pdf_close) and writes its length to
 * *outlen.  NULL on an unsupported filter or a cap breach, with the error
 * already recorded. */
const uint8_t *pdf_stream_data(TkPdfDoc *doc, PdfObj *stream, size_t *outlen);

/* Record an error.  `kind` is one of the TK_PDF_E_* constants in pdf.h. */
void pdf_fail(const char *kind, const char *fmt, ...);

/* ── Lexer ───────────────────────────────────────────────────────────────
 *
 * Shared with pdftext.c rather than duplicated, because a content stream is
 * written in the same object syntax as the file body — same numbers, same
 * names, same strings with the same escapes.  Two lexers would be two
 * chances to disagree about what `(a\)b)` means.
 */
typedef struct { const uint8_t *p; size_t n; size_t i; } PdfLex;

/* Advance past whitespace AND '%' comments, which are legal anywhere. */
void pdf_skipws(PdfLex *L);

/* Parse one object.  Returns NULL at end of input or on a token that does
 * not begin an object (an operator, `]`, `>>`, `endobj`), leaving the
 * cursor untouched so the caller can read it as a keyword. */
PdfObj *pdf_parse(TkPdfDoc *doc, PdfLex *L, unsigned depth);

/* Read a bare keyword/operator token (`Tj`, `T*`, `endstream`, `true`).
 * Returns its length, 0 at end of input.  Always NUL-terminates. */
size_t pdf_token(PdfLex *L, char *buf, size_t bufsz);

/* Does the byte at the cursor begin an object rather than an operator? */
int pdf_starts_object(const PdfLex *L);

/* Build a pooled object of the given kind. */
PdfObj *pdf_newobj(TkPdfDoc *doc, PdfKind kind);

/* Pool allocation: freed with the document, never individually. */
void *pdf_alloc(TkPdfDoc *doc, size_t n);

/* The page dictionary for 1-based page `n`, or NULL. */
PdfObj *pdf_page_dict(TkPdfDoc *doc, uint32_t n);

/* An inherited page attribute (/Resources, /MediaBox, /Rotate): looked up
 * on the page, then up the /Parent chain.  PDF says these inherit and a
 * reader that only looks at the page gets NULL for a great many real
 * documents. */
PdfObj *pdf_page_inherited(TkPdfDoc *doc, PdfObj *page, const char *key);

/* pdftext.c's entry point: walk `content` (already decoded) in the graphics
 * state established by `resources`, appending runs.  Implemented in
 * pdftext.c, called from pdf.c. */
int pdf_extract_runs(TkPdfDoc *doc, PdfObj *resources,
                     const uint8_t *content, size_t len,
                     uint32_t pageno, TkPdfRun **runs, uint32_t *nruns,
                     uint32_t *cap);

#endif /* TK_STDLIB_PDFOBJ_H */
