/*
 * test_dataframe.c — Unit tests for the std.dataframe C library (Story 16.1.3).
 *
 * Build and run: make test-stdlib-dataframe
 *
 * Tests:
 *   T01  df_fromcsv with header: 3 columns, correct nrows
 *   T02  f64 column detected correctly (values parseable as double)
 *   T03  str column detected correctly (non-numeric values)
 *   T04  df_column by name returns correct col
 *   T05  df_column missing name returns NULL
 *   T06  df_filter on f64 col > threshold: fewer rows, all above threshold
 *   T07  df_head(df, 2) returns exactly 2 rows
 *   T08  df_shape returns correct nrows and ncols
 *   T09  df_tojson is non-NULL, starts with '[', ends with ']', contains '{'
 *   T10  df_groupby count: correct number of groups
 *   T11  df_groupby sum: correct total
 *   T12  df_join combines cols from both frames
 *   T13  df_join unmatched rows are not included (inner join)
 *   T14  df_fromcsv no-header mode generates synthetic column names
 *   T15  df_filter op <= (op=1) keeps threshold value itself
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../src/stdlib/dataframe.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
        else          { printf("pass: %s\n", (msg)); } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a), (b)) == 0, (msg))

/* -------------------------------------------------------------------------
 * Sample CSV data
 * ------------------------------------------------------------------------- */

/* 3 columns: name (str), age (f64), score (f64)
 * 4 data rows */
static const char *CSV_WITH_HEADER =
    "name,age,score\r\n"
    "Alice,30,95.5\r\n"
    "Bob,25,82.0\r\n"
    "Carol,30,90.0\r\n"
    "Dave,22,78.5\r\n";

/* No header — 2 columns, 3 rows */
static const char *CSV_NO_HEADER =
    "foo,1\r\n"
    "bar,2\r\n"
    "baz,3\r\n";

/* Left frame for join: key + value */
static const char *CSV_LEFT =
    "id,left_val\r\n"
    "A,10\r\n"
    "B,20\r\n"
    "C,30\r\n";

/* Right frame for join: key + another value
 * C is missing → unmatched; D is only in right → unmatched */
static const char *CSV_RIGHT =
    "id,right_val\r\n"
    "A,100\r\n"
    "B,200\r\n"
    "D,400\r\n";

/* GroupBy CSV: category, value */
static const char *CSV_GROUP =
    "cat,val\r\n"
    "X,1\r\n"
    "Y,2\r\n"
    "X,3\r\n"
    "Z,4\r\n"
    "Y,6\r\n";

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static TkDataframe *parse_hdr(const char *csv)
{
    uint64_t len = (uint64_t)strlen(csv);
    DfResult r   = df_fromcsv(csv, len, 1);
    ASSERT(!r.is_err, "parse_hdr: df_fromcsv succeeded");
    return r.ok;
}

/* -------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

static void t01_fromcsv_header_shape(void)
{
    uint64_t len = (uint64_t)strlen(CSV_WITH_HEADER);
    DfResult r   = df_fromcsv(CSV_WITH_HEADER, len, 1);
    ASSERT(!r.is_err,        "T01: df_fromcsv no error");
    ASSERT(r.ok != NULL,     "T01: df_fromcsv returns non-NULL frame");
    ASSERT(r.ok->ncols == 3, "T01: 3 columns");
    ASSERT(r.ok->nrows == 4, "T01: 4 data rows");
    df_free(r.ok);
}

static void t02_f64_col_detected(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    DfCol *age = df_column(df, "age");
    ASSERT(age != NULL,                "T02: 'age' column found");
    ASSERT(age->type == DF_COL_F64,    "T02: 'age' type is DF_COL_F64");
    ASSERT(age->f64_data != NULL,      "T02: 'age' f64_data non-NULL");
    /* spot check: Alice age == 30 */
    ASSERT(age->f64_data[0] == 30.0,   "T02: age[0] == 30.0");
    df_free(df);
}

static void t03_str_col_detected(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    DfCol *name = df_column(df, "name");
    ASSERT(name != NULL,               "T03: 'name' column found");
    ASSERT(name->type == DF_COL_STR,   "T03: 'name' type is DF_COL_STR");
    ASSERT(name->str_data != NULL,     "T03: 'name' str_data non-NULL");
    ASSERT_STREQ(name->str_data[0], "Alice", "T03: name[0] == Alice");
    df_free(df);
}

static void t04_column_by_name(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    DfCol *score = df_column(df, "score");
    ASSERT(score != NULL,              "T04: df_column finds 'score'");
    ASSERT(score->type == DF_COL_F64,  "T04: 'score' is f64");
    ASSERT(score->f64_data[0] == 95.5, "T04: score[0] == 95.5");
    df_free(df);
}

static void t05_column_missing_returns_null(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    DfCol *missing = df_column(df, "nonexistent_column_xyz");
    ASSERT(missing == NULL, "T05: df_column returns NULL for missing name");
    df_free(df);
}

static void t06_filter_gt(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    /* age > 24 → should keep Alice(30), Bob(25), Carol(30), exclude Dave(22) */
    TkDataframe *filtered = df_filter(df, "age", 24.0, 4 /* > */);
    ASSERT(filtered != NULL,           "T06: df_filter returns non-NULL");
    ASSERT(filtered->nrows == 3,       "T06: 3 rows pass age > 24");
    /* verify all ages are > 24 */
    DfCol *age = df_column(filtered, "age");
    int all_above = 1;
    for (uint64_t r = 0; r < filtered->nrows; r++)
        if (age->f64_data[r] <= 24.0) { all_above = 0; break; }
    ASSERT(all_above, "T06: all filtered ages > 24");
    df_free(filtered);
    df_free(df);
}

static void t07_head(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    TkDataframe *h = df_head(df, 2);
    ASSERT(h != NULL,         "T07: df_head returns non-NULL");
    ASSERT(h->nrows == 2,     "T07: df_head(df,2) has 2 rows");
    ASSERT(h->ncols == df->ncols, "T07: column count preserved");
    df_free(h);
    df_free(df);
}

static void t08_shape(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    uint64_t nr, nc;
    df_shape(df, &nr, &nc);
    ASSERT(nr == 4, "T08: df_shape nrows == 4");
    ASSERT(nc == 3, "T08: df_shape ncols == 3");
    df_free(df);
}

static void t09_tojson(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    const char *json = df_tojson(df);
    ASSERT(json != NULL,         "T09: df_tojson returns non-NULL");
    ASSERT(json[0] == '[',       "T09: JSON starts with '['");
    size_t jlen = strlen(json);
    ASSERT(jlen > 0 && json[jlen - 1] == ']', "T09: JSON ends with ']'");
    ASSERT(strchr(json, '{') != NULL,          "T09: JSON contains '{'");
    free((char *)json);
    df_free(df);
}

static void t10_groupby_count(void)
{
    uint64_t len    = (uint64_t)strlen(CSV_GROUP);
    DfResult r      = df_fromcsv(CSV_GROUP, len, 1);
    if (r.is_err || !r.ok) { ASSERT(0, "T10: df_fromcsv group csv"); return; }
    TkDataframe *df = r.ok;
    DfGroupResult gr = df_groupby(df, "cat", "val", 0 /* count */);
    /* X appears 2 times, Y appears 2 times, Z appears 1 time → 3 groups */
    ASSERT(gr.nrows == 3, "T10: groupby produces 3 groups");
    /* clean up group result */
    for (uint64_t i = 0; i < gr.nrows; i++) free((char *)gr.rows[i].group_val);
    free(gr.rows);
    df_free(df);
}

static void t11_groupby_sum(void)
{
    uint64_t len    = (uint64_t)strlen(CSV_GROUP);
    DfResult r      = df_fromcsv(CSV_GROUP, len, 1);
    if (r.is_err || !r.ok) { ASSERT(0, "T11: df_fromcsv group csv"); return; }
    TkDataframe *df = r.ok;
    DfGroupResult gr = df_groupby(df, "cat", "val", 1 /* sum */);
    ASSERT(gr.nrows == 3, "T11: groupby sum has 3 groups");

    /* Total sum across all groups should equal 1+2+3+4+6 = 16 */
    double total = 0.0;
    for (uint64_t i = 0; i < gr.nrows; i++) total += gr.rows[i].agg_result;
    ASSERT(total == 16.0, "T11: groupby sum total == 16");

    for (uint64_t i = 0; i < gr.nrows; i++) free((char *)gr.rows[i].group_val);
    free(gr.rows);
    df_free(df);
}

static void t12_join_combines_cols(void)
{
    uint64_t ll = (uint64_t)strlen(CSV_LEFT);
    uint64_t rl = (uint64_t)strlen(CSV_RIGHT);
    DfResult lr = df_fromcsv(CSV_LEFT,  ll, 1);
    DfResult rr = df_fromcsv(CSV_RIGHT, rl, 1);
    if (lr.is_err || !lr.ok || rr.is_err || !rr.ok) {
        ASSERT(0, "T12: parsing join CSVs");
        df_free(lr.ok); df_free(rr.ok);
        return;
    }
    DfResult jr = df_join(lr.ok, rr.ok, "id");
    ASSERT(!jr.is_err,     "T12: df_join no error");
    ASSERT(jr.ok != NULL,  "T12: df_join returns frame");
    /* Result should have: id, left_val, right_val = 3 cols */
    ASSERT(jr.ok->ncols == 3, "T12: joined frame has 3 columns");
    /* id, left_val from left; right_val from right */
    ASSERT(df_column(jr.ok, "id")        != NULL, "T12: 'id' in result");
    ASSERT(df_column(jr.ok, "left_val")  != NULL, "T12: 'left_val' in result");
    ASSERT(df_column(jr.ok, "right_val") != NULL, "T12: 'right_val' in result");
    df_free(jr.ok);
    df_free(lr.ok);
    df_free(rr.ok);
}

static void t13_join_excludes_unmatched(void)
{
    uint64_t ll = (uint64_t)strlen(CSV_LEFT);
    uint64_t rl = (uint64_t)strlen(CSV_RIGHT);
    DfResult lr = df_fromcsv(CSV_LEFT,  ll, 1);
    DfResult rr = df_fromcsv(CSV_RIGHT, rl, 1);
    if (lr.is_err || !lr.ok || rr.is_err || !rr.ok) {
        ASSERT(0, "T13: parsing join CSVs");
        df_free(lr.ok); df_free(rr.ok);
        return;
    }
    DfResult jr = df_join(lr.ok, rr.ok, "id");
    if (jr.is_err || !jr.ok) { ASSERT(0, "T13: df_join"); return; }
    /* A and B match → 2 rows; C (left-only) and D (right-only) excluded */
    ASSERT(jr.ok->nrows == 2, "T13: inner join has exactly 2 rows");
    df_free(jr.ok);
    df_free(lr.ok);
    df_free(rr.ok);
}

static void t14_fromcsv_no_header(void)
{
    uint64_t len = (uint64_t)strlen(CSV_NO_HEADER);
    DfResult r   = df_fromcsv(CSV_NO_HEADER, len, 0 /* no header */);
    ASSERT(!r.is_err,        "T14: no-header parse succeeds");
    ASSERT(r.ok != NULL,     "T14: no-header frame non-NULL");
    ASSERT(r.ok->ncols == 2, "T14: 2 columns");
    ASSERT(r.ok->nrows == 3, "T14: 3 rows");
    /* Column names should be col0, col1 */
    ASSERT(df_column(r.ok, "col0") != NULL, "T14: col0 exists");
    ASSERT(df_column(r.ok, "col1") != NULL, "T14: col1 exists");
    df_free(r.ok);
}

static void t15_filter_lte(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    /* age <= 25 → Bob(25) and Dave(22) */
    TkDataframe *filtered = df_filter(df, "age", 25.0, 1 /* <= */);
    ASSERT(filtered != NULL,      "T15: df_filter <= returns non-NULL");
    ASSERT(filtered->nrows == 2,  "T15: age <= 25 yields 2 rows");
    DfCol *age = df_column(filtered, "age");
    int all_ok = 1;
    for (uint64_t r = 0; r < filtered->nrows; r++)
        if (age->f64_data[r] > 25.0) { all_ok = 0; break; }
    ASSERT(all_ok, "T15: all filtered ages <= 25");
    df_free(filtered);
    df_free(df);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    t01_fromcsv_header_shape();
    t02_f64_col_detected();
    t03_str_col_detected();
    t04_column_by_name();
    t05_column_missing_returns_null();
    t06_filter_gt();
    t07_head();
    t08_shape();
    t09_tojson();
    t10_groupby_count();
    t11_groupby_sum();
    t12_join_combines_cols();
    t13_join_excludes_unmatched();
    t14_fromcsv_no_header();
    t15_filter_lte();

    printf("\n%s — %d failure(s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
