/* SPDX-License-Identifier: MIT */
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

#include "tk_runtime.h"   /* 127.238: tk_current_error + tk_err_box */
#include "tk_strhdr.h"  /* 127.295: sealed strings, tk_strlen */

/* 127.238: the error slot (runtime-abi.md §7.2).  These `T!dferr` wrappers
 * returned a bare 0 on failure, so only the value sentinel saw it and the
 * $err arm bound nil (`e.msg` was RT005).  The failure value is unchanged
 * (0; every T here is a handle or pointer); every return now goes through
 * df_slot, which on 0 also stores a dferr record {msg; …} — the other
 * fields 0 — and on anything else clears the slot, so the wrapper is a slot
 * wrapper (llvm.c derives them from the .tki). */
static int64_t df_slot(int64_t v, const char *msg) {
    if (v) { tk_current_error = 0; return v; }
    int64_t *box = (int64_t *)tk_err_box(1 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)msg;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* 138.63: the registry (tk_runtime.h) vouches for every $dataframe a toke
 * program holds.  A dataframe is never released from toke, so it is its
 * address, registered wherever this file hands one out (df_out); every
 * wrapper looks its argument up first (df_in), so a value the runtime did
 * not hand out -- `5 as $dataframe`, a record literal -- is the null
 * dataframe and is never read. */
static TkDataframe *df_in(int64_t v) { return (TkDataframe *)tk_handle_addr(TK_HK_DATAFRAME, v); }
static int64_t df_out(TkDataframe *d)
{
    if (!d) return 0;
    if (!tk_handle_adopt(TK_HK_DATAFRAME, d)) { df_free(d); return 0; }
    return (int64_t)(intptr_t)d;
}

int64_t tk_dataframe_fromrows_w(int64_t headers, int64_t rows) {
    int64_t ncols = tk_arr_len(headers);
    int64_t nrows = tk_arr_len(rows);
    if (ncols <= 0) return df_out(df_new());

    const char **hdr = (const char **)malloc((size_t)ncols * sizeof(char *));
    if (!hdr) return df_out(df_new());
    int64_t *hs = (int64_t *)(intptr_t)headers;
    for (int64_t i = 0; i < ncols; i++)
        hdr[i] = hs[i] ? (const char *)(intptr_t)hs[i] : "";

    const char ***cells = NULL;
    if (nrows > 0) {
        cells = (const char ***)malloc((size_t)nrows * sizeof(char **));
        if (!cells) { free(hdr); return df_out(df_new()); }
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
    return df_out(out ? out : df_new());
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
    uint64_t len = tk_strlen(s);
    DfResult r = df_fromcsv(s, len, 1);
    if (r.is_err || !r.ok) return 0;
    return df_out(r.ok);
}

/* 138.63: the null dataframe's shape is {0; 0}, a record toke can read; it
 * was 0, which `.rows` read as the null record (nil trap). */
static int64_t df_shape_impl(int64_t df_ptr) {
    TkDataframe *d = df_in(df_ptr);
    uint64_t nrows = 0, ncols = 0;
    if (d) df_shape(d, &nrows, &ncols);
    /* Pack nrows and ncols into a heap block */
    int64_t *block = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)nrows;
    block[1] = (int64_t)ncols;
    return (int64_t)(intptr_t)block;
}

/* 138.66: df.filter(dataframe;str;str;str) -- op and val are toke strs, as
 * the .tki and docs/stdlib/dataframe.md declare.  The glue read op as
 * df_filter's int operator code and val's pointer bits as an f64, so every
 * call was $err.  op is one of the documented "<" "<=" "=" ">=" ">" "!="
 * (exactly; anything else is $err), and val must parse as an f64 in full
 * (strtod, no trailing bytes; "" is $err). */
static int df_filter_op(const char *op)
{
    static const char *const ops[] = { "<", "<=", "=", ">=", ">", "!=" };
    for (int i = 0; i < 6; i++)
        if (strcmp(op, ops[i]) == 0) return i;
    return -1;
}

static int64_t df_filter_impl(int64_t df_ptr, int64_t col, int64_t op, int64_t val) {
    TkDataframe *d = df_in(df_ptr);
    if (!d || !col || !op || !val) return 0;
    int code = df_filter_op((const char *)(intptr_t)op);
    if (code < 0) return 0;
    const char *vs = (const char *)(intptr_t)val;
    uint64_t vlen = tk_strlen(vs);
    if (vlen == 0 || memchr(vs, '\0', (size_t)vlen)) return 0;
    char *end = NULL;
    double threshold = strtod(vs, &end);
    if (end != vs + vlen) return 0;
    TkDataframe *result = df_filter(d, (const char *)(intptr_t)col,
                                     threshold, code);
    return df_out(result);
}

static int64_t df_groupby_impl(int64_t df_ptr, int64_t col) {
    TkDataframe *d = df_in(df_ptr);
    if (!d || !col) return 0;
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
    TkDataframe *d = df_in(df_ptr);
    if (!d || !col) return 0;
    uint64_t out_len = 0;
    char **strs = df_columnstr(d, (const char *)(intptr_t)col, &out_len);
    if (!strs) return 0;
    return (int64_t)(intptr_t)strs;
}

static const char tk_dataframe_fromcsv_fail[] =
    "df.fromcsv: the CSV could not be read as a dataframe";
int64_t tk_dataframe_fromcsv_w(int64_t csv) { return df_slot(df_fromcsv_impl(csv), tk_dataframe_fromcsv_fail); }
int64_t tk_dataframe_shape_w(int64_t df) { return df_shape_impl(df); }
static const char tk_dataframe_filter_fail[] =
    "df.filter: the filter could not be applied (unknown or non-numeric column, unknown op, or a val that is not an f64)";
int64_t tk_dataframe_filter_w(int64_t df, int64_t col, int64_t op, int64_t val) { return df_slot(df_filter_impl(df, col, op, val), tk_dataframe_filter_fail); }
static const char tk_dataframe_groupby_fail[] =
    "df.groupby: the column could not be grouped";
int64_t tk_dataframe_groupby_w(int64_t df, int64_t col) { return df_slot(df_groupby_impl(df, col), tk_dataframe_groupby_fail); }
static const char tk_dataframe_columnstr_fail[] =
    "df.columnstr: no such string column";
int64_t tk_dataframe_columnstr_w(int64_t df, int64_t col) { return df_slot(df_columnstr_impl(df, col), tk_dataframe_columnstr_fail); }
/* 127.295: df.tocsv hands toke a sealed copy of df_tocsv's malloc'd text
 * (freed here); the original is handed over unsealed only if the copy
 * cannot be made.  No C code calls tk_dataframe_tocsv_w. */
int64_t tk_dataframe_tocsv_w(int64_t df) {
    TkDataframe *d = df_in(df);
    if (!d) return 0;
    const char *s = df_tocsv(d);
    if (!s) return 0;
    char *t = tks_strdup(s);
    if (!t) return (int64_t)(intptr_t)s;
    free((void *)s);
    return (int64_t)(intptr_t)t;
}
