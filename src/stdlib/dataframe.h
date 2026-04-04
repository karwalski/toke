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

#endif /* TK_STDLIB_DATAFRAME_H */
