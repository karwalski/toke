#ifndef TK_STDLIB_DATAFRAME_H
#define TK_STDLIB_DATAFRAME_H

/*
 * dataframe.h — C interface for the std.dataframe standard library module.
 *
 * Type mappings:
 *   Str      = const char *  (null-terminated UTF-8)
 *   f64      = double
 *   u64      = uint64_t
 *
 * malloc is permitted; callers own returned pointers unless noted otherwise.
 * Use df_free() to release a TkDataframe and all its contents.
 *
 * No external dependencies beyond libc and csv.h.
 *
 * Story: 16.1.3
 */

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Column types
 * ------------------------------------------------------------------------- */

typedef enum {
    DF_COL_F64,   /* numeric column; data lives in f64_data */
    DF_COL_STR    /* string column;  data lives in str_data */
} DfColType;

typedef struct {
    const char *name;       /* heap-allocated, NUL-terminated column name */
    DfColType   type;
    double     *f64_data;   /* used if type == DF_COL_F64; nrows elements */
    char      **str_data;   /* used if type == DF_COL_STR;  nrows elements */
    uint64_t    nrows;
} DfCol;

/* -------------------------------------------------------------------------
 * Dataframe
 * ------------------------------------------------------------------------- */

typedef struct {
    DfCol    *cols;   /* array of ncols columns */
    uint64_t  ncols;
    uint64_t  nrows;
} TkDataframe;

/* -------------------------------------------------------------------------
 * Result type (used by operations that can fail)
 * ------------------------------------------------------------------------- */

typedef struct {
    TkDataframe *ok;        /* non-NULL on success */
    int          is_err;    /* non-zero on error   */
    const char  *err_msg;   /* static or heap string; valid when is_err!=0 */
} DfResult;

/* -------------------------------------------------------------------------
 * Core operations
 * ------------------------------------------------------------------------- */

/* df_new: allocate an empty dataframe with zero columns/rows. */
TkDataframe *df_new(void);

/* df_fromcsv: parse csv_data (csv_len bytes) into a new dataframe.
 * If has_header!=0 the first row is used as column names; otherwise columns
 * are named "col0", "col1", …
 * Columns where every value is parseable as f64 (via strtod) become
 * DF_COL_F64; all others become DF_COL_STR. */
DfResult     df_fromcsv(const char *csv_data, uint64_t csv_len, int has_header);

/* df_column: return a pointer to the named column, or NULL if not found.
 * The pointer is valid for the lifetime of df. */
DfCol       *df_column(TkDataframe *df, const char *name);

/* df_filter: return a new dataframe containing only rows where the f64
 * column named col satisfies (value OP threshold).
 * op: 0=<  1=<=  2===  3=>=  4=>
 * Returns NULL if col is not found or is not DF_COL_F64. */
TkDataframe *df_filter(TkDataframe *df, const char *col,
                       double threshold, int op);

/* df_head: return a new dataframe with at most the first n rows. */
TkDataframe *df_head(TkDataframe *df, uint64_t n);

/* df_shape: write the row and column counts into the provided pointers. */
void         df_shape(TkDataframe *df, uint64_t *nrows_out, uint64_t *ncols_out);

/* df_tojson: serialise the dataframe as a JSON array of row objects.
 * f64 columns emit numeric values; str columns emit quoted strings.
 * Returns a heap-allocated NUL-terminated string; caller owns it.
 * Format: [{"col1":val,"col2":"str",...},...] */
const char  *df_tojson(TkDataframe *df);

/* df_free: release all memory owned by df, including df itself. */
void         df_free(TkDataframe *df);

/* df_join: hash join on a shared string column on_col.
 * Rows in left and right with the same key value are combined.
 * Unmatched rows are excluded (inner join).
 * The key column appears once in the result. */
DfResult     df_join(TkDataframe *left, TkDataframe *right, const char *on_col);

/* df_fromrows: build a dataframe from column names and row-major string data.
 * headers is an array of ncols column names; rows is an array of nrows
 * StrArrays, each of length ncols.  Columns where every value is parseable
 * as f64 become DF_COL_F64; all others become DF_COL_STR.
 * Returns a heap-allocated dataframe; caller owns it via df_free(). */
TkDataframe *df_fromrows(const char **headers, uint64_t ncols,
                          const char ***rows, uint64_t nrows);

/* df_columnstr: return an array of heap-allocated strings for the named
 * string column.  Sets *out_len to the number of elements.
 * Returns NULL if the column is not found or is not DF_COL_STR.
 * Caller owns the returned array and each string in it. */
char       **df_columnstr(TkDataframe *df, const char *name, uint64_t *out_len);

/* df_tocsv: serialise the dataframe as RFC 4180 CSV with a header row.
 * Returns a heap-allocated NUL-terminated string; caller owns it. */
const char  *df_tocsv(TkDataframe *df);

/* df_schema: return column metadata.  Sets *out_len to the number of columns.
 * Each DfSeries describes one column (name, type string, length).
 * Caller owns the returned array and each name/dtype string in it. */
typedef struct {
    char     *name;     /* heap-allocated column name */
    char     *dtype;    /* "f64" or "str"             */
    uint64_t  len;      /* number of rows             */
} DfSeries;

DfSeries    *df_schema(TkDataframe *df, uint64_t *out_len);

/* -------------------------------------------------------------------------
 * GroupBy + aggregate
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *group_val;    /* unique value from group_col */
    double      agg_result;   /* result of the aggregation   */
} DfGroupRow;

typedef struct {
    DfGroupRow *rows;
    uint64_t    nrows;
} DfGroupResult;

/* df_groupby: group df by the string column group_col and aggregate the
 * f64 column agg_col.
 * agg: 0=count  1=sum  2=mean
 * Returns a DfGroupResult with heap-allocated rows; caller owns it. */
DfGroupResult df_groupby(TkDataframe *df, const char *group_col,
                         const char *agg_col, int agg);

/* -------------------------------------------------------------------------
 * Sort, unique, and column operations  (Story 31.1.1)
 * ------------------------------------------------------------------------- */

/* df_sort: return a new dataframe with rows sorted by the values in col.
 * For numeric-looking values, sorts numerically; otherwise sorts as strings.
 * ascending=1 for A→Z / 0→9; ascending=0 for the reverse.
 * Returns NULL if col is not found. */
TkDataframe *df_sort(TkDataframe *df, const char *col, int ascending);

/* df_unique: return a new dataframe keeping only the first occurrence of each
 * distinct value in col, preserving original row order.
 * Returns NULL if col is not found. */
TkDataframe *df_unique(TkDataframe *df, const char *col);

/* df_drop_column: return a new dataframe without the named column.
 * If col is not found, returns a copy of the original. */
TkDataframe *df_drop_column(TkDataframe *df, const char *col);

/* df_rename_column: return a new dataframe with old_name renamed to new_name.
 * If old_name is not found, returns a copy of the original. */
TkDataframe *df_rename_column(TkDataframe *df, const char *old_name,
                               const char *new_name);

/* df_select_columns: return a new dataframe containing only the columns listed
 * in cols (in the given order).  Columns not found are silently skipped.
 * Returns NULL if df or cols is NULL. */
TkDataframe *df_select_columns(TkDataframe *df, const char **cols,
                                uint64_t ncols);

/* df_value_counts: return a DfGroupResult where each group key is a unique
 * value in col and agg_result is the number of rows with that value.
 * The group_col must be a DF_COL_STR column.
 * Returns an empty result (nrows==0, rows==NULL) on error. */
DfGroupResult df_value_counts(TkDataframe *df, const char *col);

#endif /* TK_STDLIB_DATAFRAME_H */
