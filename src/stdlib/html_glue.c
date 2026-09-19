/*
 * html_glue.c — i64-ABI wrappers for std.html (Story 114.35).
 * Extracted from tk_web_glue.c so a standalone `i=html:std.html` import links
 * the wrappers (they previously only linked when std.http pulled in
 * tk_web_glue.c). Same fix class as 114.31 (net_glue.c).
 */
#include "html.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * html.doc() takes no arguments -- stdlib/html.tki says so, docs/stdlib/html.md
 * says so, and llvm.c DECLARES it so ("declare i64 @tk_html_doc_w()"). The
 * definition took an int64_t title and dereferenced it when non-zero, so the
 * callee read whatever the ABI's first argument register happened to hold and
 * passed it to html_title as a char *. Every `let doc = html.doc();` -- the
 * first line of the module's only documented example -- segfaulted. Found
 * while proving 136.24; reported for its own row.
 */
int64_t tk_html_doc_w(void) {
    TkHtmlDoc *doc = html_doc();
    if (!doc) return 0;
    return (int64_t)(intptr_t)doc;
}

int64_t tk_html_h1_w(int64_t text) {
    TkHtmlNode *node = html_h1((const char *)(intptr_t)text);
    return (int64_t)(intptr_t)node;
}

/*
 * Story 136.24 — html.table(headers; rows).
 *
 * stdlib/html.tki, the function table in docs/stdlib/html.md and html_table()
 * in html.h all take the headers and the row data as two separate arguments,
 * and html_table() builds <thead> from the first and <tbody> from the second.
 * The wrapper took ONE array and treated its first element as the header row,
 * so a caller had no way to say "this table has these headers and these
 * rows" -- the two were flattened into one list and the distinction was
 * recovered by position. A table whose first data row happened to be passed
 * first silently became its own header.
 *
 * html_table() strdup()s every cell, so the two index arrays are ours to free.
 */
int64_t tk_html_table_w(int64_t headers_i64, int64_t rows_i64) {
    /* headers_i64: toke array of strings   (hp[-1]=ncols, hp[0..]=str ptrs)
     * rows_i64:    toke array of arrays    (rp[-1]=nrows, each element is a
     *              toke array of strings)  */
    if (!headers_i64) return 0;
    int64_t *hp = (int64_t *)(intptr_t)headers_i64;
    int64_t ncols = hp[-1];
    if (ncols <= 0) return 0;

    const char **headers = (const char **)malloc((size_t)ncols * sizeof(const char *));
    if (!headers) return 0;
    for (int64_t c = 0; c < ncols; c++)
        headers[c] = (const char *)(intptr_t)hp[c];

    int64_t nrows = 0;
    int64_t *rows = NULL;
    if (rows_i64) {
        rows = (int64_t *)(intptr_t)rows_i64;
        nrows = rows[-1];
        if (nrows < 0) nrows = 0;
    }

    const char **cells = NULL;
    if (nrows > 0) {
        cells = (const char **)malloc((size_t)(nrows * ncols) * sizeof(const char *));
        if (!cells) { free(headers); return 0; }
        for (int64_t r = 0; r < nrows; r++) {
            int64_t *rp = (int64_t *)(intptr_t)rows[r];
            int64_t rc = rp ? rp[-1] : 0;
            for (int64_t c = 0; c < ncols; c++)
                cells[r * ncols + c] = (c < rc) ? (const char *)(intptr_t)rp[c] : "";
        }
    }

    TkHtmlNode *node = html_table(headers, (uint64_t)ncols,
                                  cells, (uint64_t)nrows);
    free(headers);
    free(cells);
    return (int64_t)(intptr_t)node;
}

int64_t tk_html_render_w(int64_t doc) {
    if (!doc) return 0;
    const char *s = html_render((TkHtmlDoc *)(intptr_t)doc);
    return (int64_t)(intptr_t)s;
}

int64_t tk_html_style_w(int64_t doc, int64_t css) {
    if (!doc) return 0;
    html_style((TkHtmlDoc *)(intptr_t)doc, (const char *)(intptr_t)css);
    return doc;
}

int64_t tk_html_title_w(int64_t doc, int64_t t) {
    if (!doc) return 0;
    html_title((TkHtmlDoc *)(intptr_t)doc, (const char *)(intptr_t)t);
    return doc;
}

int64_t tk_html_append_w(int64_t doc, int64_t elem) {
    if (!doc || !elem) return doc;
    html_append((TkHtmlDoc *)(intptr_t)doc, (TkHtmlNode *)(intptr_t)elem);
    return doc;
}

int64_t tk_html_docr_w(int64_t body) {
    /* Convenience: create doc, append body text as a raw paragraph, render */
    TkHtmlDoc *doc = html_doc();
    if (!doc) return 0;
    if (body) {
        TkHtmlNode *p = html_p((const char *)(intptr_t)body);
        if (p) html_append(doc, p);
    }
    const char *s = html_render(doc);
    return (int64_t)(intptr_t)s;
}
