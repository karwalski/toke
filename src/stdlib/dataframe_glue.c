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
