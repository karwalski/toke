/*
 * dataframe_glue.c — i64-ABI wrapper for df.fromrows (Story 136.32).
 *
 * df_fromrows() has been in dataframe.c and declared in dataframe.h since the
 * module shipped, and stdlib/dataframe.tki exports `df.fromrows([str];
 * [csvrow]) : dataframe`. It is the constructor docs/stdlib/analytics.md
 * reaches for in the $err arm of every one of its eight examples, and it had
 * no wrapper at all — unlike df.fromcsv/filter/tocsv/columnstr/groupby/shape,
 * whose wrappers exist but are stranded in tk_web_glue.c (story 136.33, not
 * touched here).
 *
 * `[csvrow]` is the layout tk_csv_parse_w produces: an array of $csvrow
 * handles, each a one-slot block holding a `[str]` array of field values.
 * `df.fromrows(@(); @())` — the empty call the documentation uses — yields an
 * empty dataframe rather than a null handle, because the declared return type
 * is a plain `dataframe` with no error arm to carry a failure.
 */
#include "dataframe.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_array.h"

int64_t tk_dataframe_fromrows_w(int64_t headers, int64_t rows) {
    int64_t ncols = tk_arr_len(headers);
    int64_t nrows = tk_arr_len(rows);
    if (ncols <= 0) return (int64_t)(intptr_t)df_new();

    const char **hdr = (const char **)malloc((size_t)ncols * sizeof(char *));
    if (!hdr) return (int64_t)(intptr_t)df_new();
    int64_t *hs = (int64_t *)(intptr_t)headers;
    for (int64_t i = 0; i < ncols; i++)
        hdr[i] = hs[i] ? (const char *)(intptr_t)hs[i] : "";

    const char ***cells = NULL;
    if (nrows > 0) {
        cells = (const char ***)malloc((size_t)nrows * sizeof(char **));
        if (!cells) { free(hdr); return (int64_t)(intptr_t)df_new(); }
        int64_t *rs = (int64_t *)(intptr_t)rows;
        for (int64_t r = 0; r < nrows; r++) {
            const char **row = (const char **)calloc((size_t)ncols, sizeof(char *));
            /* every cell defaults to "" so a short row cannot read past its end */
            for (int64_t c = 0; c < ncols; c++) if (row) row[c] = "";
            int64_t fields = rs[r] ? ((int64_t *)(intptr_t)rs[r])[0] : 0;
            int64_t nf = tk_arr_len(fields);
            if (row && fields) {
                int64_t *fs = (int64_t *)(intptr_t)fields;
                for (int64_t c = 0; c < ncols && c < nf; c++)
                    if (fs[c]) row[c] = (const char *)(intptr_t)fs[c];
            }
            cells[r] = row;
        }
    }

    TkDataframe *out = df_fromrows(hdr, (uint64_t)ncols, cells, (uint64_t)nrows);
    if (cells) {
        for (int64_t r = 0; r < nrows; r++) free((void *)cells[r]);
        free(cells);
    }
    free(hdr);
    return (int64_t)(intptr_t)(out ? out : df_new());
}

/* `df.` and `dataframe.` are both live aliases for this module (see the
 * paired tk_df_ and tk_dataframe_ wrappers in tk_web_glue.c), so both
 * spellings resolve. */
int64_t tk_df_fromrows_w(int64_t headers, int64_t rows) {
    return tk_dataframe_fromrows_w(headers, rows);
}

/* ─────────────────────────────────────────────────────────────────────
 * 136.40 — moved verbatim from tk_web_glue.c.
 *
 * These six wrappers were written and correct, but tk_web_glue.c is
 * registered against the HTTP module in src/stdlib_deps.c, so they only
 * reached the linker when the program also imported std.http. Every
 * documented std.dataframe and std.analytics example therefore
 * type-checked and then failed at link on tk_dataframe_fromcsv_w and
 * friends -- 10 of the 15 check-docs failures. The C behind them
 * (df_fromcsv/filter/groupby/columnstr/shape/tocsv in dataframe.c) has
 * been complete since the module shipped; nothing here is new code.
 *
 * The tk_df_* spellings are kept because src/stdlib_decls_gen.h still
 * declares them; only the tk_dataframe_* spellings are what
 * stdlib_symbol_for() actually emits for `df.*` exports of std.dataframe.
 * ───────────────────────────────────────────────────────────────────── */

/* ── dataframe wrappers (dataframe.h) ─────────────────────────────── */
static int64_t df_fromcsv_impl(int64_t csv) {
    if (!csv) return 0;
    const char *s = (const char *)(intptr_t)csv;
    uint64_t len = strlen(s);
    DfResult r = df_fromcsv(s, len, 1);
    if (r.is_err || !r.ok) return 0;
    return (int64_t)(intptr_t)r.ok;
}

static int64_t df_shape_impl(int64_t df_ptr) {
    if (!df_ptr) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    uint64_t nrows, ncols;
    df_shape(d, &nrows, &ncols);
    /* Pack nrows and ncols into a heap block */
    int64_t *block = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)nrows;
    block[1] = (int64_t)ncols;
    return (int64_t)(intptr_t)block;
}

static int64_t df_filter_impl(int64_t df_ptr, int64_t col, int64_t op, int64_t val) {
    if (!df_ptr || !col) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    double threshold;
    memcpy(&threshold, &val, sizeof(double));
    TkDataframe *result = df_filter(d, (const char *)(intptr_t)col,
                                     threshold, (int)op);
    return (int64_t)(intptr_t)result;
}

static int64_t df_groupby_impl(int64_t df_ptr, int64_t col) {
    if (!df_ptr || !col) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    /* Default: group by col, aggregate count on first numeric column */
    DfGroupResult gr = df_groupby(d, (const char *)(intptr_t)col, NULL, 0);
    if (!gr.rows) return 0;
    /* Return pointer to the result (caller can iterate) */
    DfGroupResult *heap = (DfGroupResult *)malloc(sizeof(DfGroupResult));
    if (!heap) return 0;
    *heap = gr;
    return (int64_t)(intptr_t)heap;
}

static int64_t df_columnstr_impl(int64_t df_ptr, int64_t col) {
    if (!df_ptr || !col) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    uint64_t out_len = 0;
    char **strs = df_columnstr(d, (const char *)(intptr_t)col, &out_len);
    if (!strs) return 0;
    return (int64_t)(intptr_t)strs;
}

int64_t tk_df_fromcsv_w(int64_t csv) { return df_fromcsv_impl(csv); }
int64_t tk_df_shape_w(int64_t df) { return df_shape_impl(df); }
int64_t tk_df_filter_w(int64_t df, int64_t col, int64_t op, int64_t val) { return df_filter_impl(df, col, op, val); }
int64_t tk_df_groupby_w(int64_t df, int64_t col) { return df_groupby_impl(df, col); }
int64_t tk_df_columnstr_w(int64_t df, int64_t col) { return df_columnstr_impl(df, col); }
int64_t tk_dataframe_fromcsv_w(int64_t csv) { return df_fromcsv_impl(csv); }
int64_t tk_dataframe_shape_w(int64_t df) { return df_shape_impl(df); }
int64_t tk_dataframe_filter_w(int64_t df, int64_t col, int64_t op, int64_t val) { return df_filter_impl(df, col, op, val); }
int64_t tk_dataframe_groupby_w(int64_t df, int64_t col) { return df_groupby_impl(df, col); }
int64_t tk_dataframe_columnstr_w(int64_t df, int64_t col) { return df_columnstr_impl(df, col); }
int64_t tk_dataframe_tocsv_w(int64_t df) {
    if (!df) return 0;
    const char *s = df_tocsv((TkDataframe *)(intptr_t)df);
    return (int64_t)(intptr_t)s;
}
