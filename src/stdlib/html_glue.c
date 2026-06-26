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

int64_t tk_html_doc_w(int64_t title) {
    TkHtmlDoc *doc = html_doc();
    if (!doc) return 0;
    if (title) html_title(doc, (const char *)(intptr_t)title);
    return (int64_t)(intptr_t)doc;
}

int64_t tk_html_h1_w(int64_t text) {
    TkHtmlNode *node = html_h1((const char *)(intptr_t)text);
    return (int64_t)(intptr_t)node;
}

int64_t tk_html_table_w(int64_t data) {
    /* data is a toke array of string arrays (rows).
     * Layout: ptr[-1] = row count, ptr[0..n-1] = row pointers.
     * Each row is itself a toke array: rptr[-1] = col count, rptr[0..m-1] = strings.
     * The first row is treated as headers. */
    if (!data) return 0;
    int64_t *rows = (int64_t *)(intptr_t)data;
    int64_t nrows = rows[-1];
    if (nrows <= 0) return 0;

    /* Extract headers from first row */
    int64_t *hdr_row = (int64_t *)(intptr_t)rows[0];
    int64_t ncols = hdr_row[-1];
    if (ncols <= 0) return 0;
    const char **headers = (const char **)malloc((size_t)ncols * sizeof(const char *));
    if (!headers) return 0;
    for (int64_t c = 0; c < ncols; c++)
        headers[c] = (const char *)(intptr_t)hdr_row[c];

    /* Extract data rows (rows 1..n-1) */
    int64_t data_nrows = nrows - 1;
    int64_t total_cells = data_nrows * ncols;
    const char **cells = NULL;
    if (total_cells > 0) {
        cells = (const char **)malloc((size_t)total_cells * sizeof(const char *));
        if (!cells) { free(headers); return 0; }
        for (int64_t r = 0; r < data_nrows; r++) {
            int64_t *rp = (int64_t *)(intptr_t)rows[r + 1];
            int64_t rc = rp[-1];
            for (int64_t c = 0; c < ncols; c++) {
                if (c < rc)
                    cells[r * ncols + c] = (const char *)(intptr_t)rp[c];
                else
                    cells[r * ncols + c] = "";
            }
        }
    }
    TkHtmlNode *node = html_table(headers, (uint64_t)ncols,
                                   cells, (uint64_t)data_nrows);
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
