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
 *   T16  df_fromrows creates dataframe with correct shape and types
 *   T17  df_columnstr returns string data for a str column
 *   T18  df_columnstr returns NULL for an f64 column
 *   T19  df_tocsv round-trips: fromcsv → tocsv produces header + data rows
 *   T20  df_schema returns correct column names and dtypes
 *   T21  Large dataframe (1000+ rows) handles scale
 *   T22  Empty dataframe operations (0 data rows)
 *   T23  Single row/column dataframe
 *   T24  Filter on nonexistent column returns NULL
 *   T25  Filter on str column returns NULL
 *   T26  Filter with no matches returns 0-row frame
 *   T27  Filter equality (op==2) returns exact matches
 *   T28  Join on missing column returns error
 *   T29  GroupBy mean aggregation
 *   T30  GroupBy on missing columns returns empty result
 *   T31  Column access out of bounds (NULL name, empty name)
 *   T32  Duplicate column names: df_column returns the first match
 *   T33  df_fromrows with zero rows
 *   T34  df_fromrows with NULL headers returns NULL
 *   T35  tocsv round-trip consistency (parse back produces same shape/values)
 *   T36  Schema on mixed column types
 *   T37  df_head with n > nrows returns all rows
 *   T38  df_head with n == 0 returns 0 rows
 *   T39  df_tojson on empty frame
 *   T40  df_shape on NULL frame
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

static void t16_fromrows(void)
{
    const char *headers[] = {"city", "pop"};
    const char *row0[] = {"Sydney", "5000000"};
    const char *row1[] = {"Melbourne", "4900000"};
    const char *row2[] = {"Brisbane", "2500000"};
    const char **rows[] = {row0, row1, row2};
    TkDataframe *df = df_fromrows(headers, 2, (const char ***)rows, 3);
    ASSERT(df != NULL,         "T16: df_fromrows returns non-NULL");
    ASSERT(df->ncols == 2,     "T16: 2 columns");
    ASSERT(df->nrows == 3,     "T16: 3 rows");
    DfCol *city = df_column(df, "city");
    ASSERT(city != NULL && city->type == DF_COL_STR, "T16: city is str column");
    ASSERT_STREQ(city->str_data[0], "Sydney", "T16: city[0] == Sydney");
    DfCol *pop = df_column(df, "pop");
    ASSERT(pop != NULL && pop->type == DF_COL_F64, "T16: pop is f64 column");
    ASSERT(pop->f64_data[0] == 5000000.0, "T16: pop[0] == 5000000");
    df_free(df);
}

static void t17_columnstr(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    uint64_t len = 0;
    char **names = df_columnstr(df, "name", &len);
    ASSERT(names != NULL, "T17: df_columnstr returns non-NULL for str col");
    ASSERT(len == 4,      "T17: 4 elements");
    ASSERT_STREQ(names[0], "Alice", "T17: names[0] == Alice");
    ASSERT_STREQ(names[3], "Dave",  "T17: names[3] == Dave");
    for (uint64_t i = 0; i < len; i++) free(names[i]);
    free(names);
    df_free(df);
}

static void t18_columnstr_f64_returns_null(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    uint64_t len = 0;
    char **result = df_columnstr(df, "age", &len);
    ASSERT(result == NULL, "T18: df_columnstr returns NULL for f64 column");
    ASSERT(len == 0,       "T18: out_len remains 0");
    df_free(df);
}

static void t19_tocsv(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    const char *csv = df_tocsv(df);
    ASSERT(csv != NULL, "T19: df_tocsv returns non-NULL");
    /* Should contain the header */
    ASSERT(strstr(csv, "name") != NULL,  "T19: CSV contains 'name' header");
    ASSERT(strstr(csv, "age") != NULL,   "T19: CSV contains 'age' header");
    ASSERT(strstr(csv, "score") != NULL, "T19: CSV contains 'score' header");
    /* Should contain data */
    ASSERT(strstr(csv, "Alice") != NULL, "T19: CSV contains 'Alice'");
    ASSERT(strstr(csv, "Bob") != NULL,   "T19: CSV contains 'Bob'");
    free((char *)csv);
    df_free(df);
}

static void t20_schema(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    uint64_t len = 0;
    DfSeries *s = df_schema(df, &len);
    ASSERT(s != NULL,  "T20: df_schema returns non-NULL");
    ASSERT(len == 3,   "T20: 3 columns in schema");
    /* name col is str, age is f64, score is f64 */
    ASSERT_STREQ(s[0].name, "name",  "T20: schema[0].name == name");
    ASSERT_STREQ(s[0].dtype, "str",  "T20: schema[0].dtype == str");
    ASSERT_STREQ(s[1].name, "age",   "T20: schema[1].name == age");
    ASSERT_STREQ(s[1].dtype, "f64",  "T20: schema[1].dtype == f64");
    ASSERT(s[0].len == 4, "T20: schema[0].len == 4");
    for (uint64_t i = 0; i < len; i++) { free(s[i].name); free(s[i].dtype); }
    free(s);
    df_free(df);
}

/* -------------------------------------------------------------------------
 * Hardening tests (Story 20.1.6)
 * ------------------------------------------------------------------------- */

static void t21_large_dataframe(void)
{
    /* Build a CSV with 1000+ rows programmatically. */
    /* Header: idx,val  —  1002 rows of data */
    size_t buf_cap = 64 * 1024;
    char *csv_buf = malloc(buf_cap);
    ASSERT(csv_buf != NULL, "T21: alloc csv buffer");
    if (!csv_buf) return;

    size_t pos = 0;
    pos += (size_t)snprintf(csv_buf + pos, buf_cap - pos, "idx,val\r\n");
    for (int i = 0; i < 1002; i++) {
        pos += (size_t)snprintf(csv_buf + pos, buf_cap - pos, "%d,%d\r\n", i, i * 10);
    }

    DfResult r = df_fromcsv(csv_buf, (uint64_t)pos, 1);
    ASSERT(!r.is_err,           "T21: large CSV parses without error");
    ASSERT(r.ok != NULL,        "T21: large CSV frame non-NULL");
    ASSERT(r.ok->nrows == 1002, "T21: 1002 data rows");
    ASSERT(r.ok->ncols == 2,    "T21: 2 columns");

    /* Verify last row */
    DfCol *idx = df_column(r.ok, "idx");
    ASSERT(idx != NULL && idx->type == DF_COL_F64, "T21: idx is f64");
    if (idx && idx->type == DF_COL_F64) {
        ASSERT(idx->f64_data[1001] == 1001.0, "T21: last row idx == 1001");
    }

    /* Filter on large frame */
    TkDataframe *filtered = df_filter(r.ok, "idx", 999.0, 3 /* >= */);
    ASSERT(filtered != NULL,       "T21: filter on large frame non-NULL");
    ASSERT(filtered->nrows == 3,   "T21: filter >= 999 yields 3 rows (999,1000,1001)");
    df_free(filtered);

    /* Head on large frame */
    TkDataframe *h = df_head(r.ok, 5);
    ASSERT(h != NULL && h->nrows == 5, "T21: head(5) on large frame");
    df_free(h);

    df_free(r.ok);
    free(csv_buf);
}

static void t22_empty_dataframe(void)
{
    /* CSV with header only, no data rows. */
    const char *csv = "a,b,c\r\n";
    DfResult r = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!r.is_err,        "T22: header-only CSV parses");
    ASSERT(r.ok != NULL,     "T22: frame non-NULL");
    ASSERT(r.ok->ncols == 3, "T22: 3 columns");
    ASSERT(r.ok->nrows == 0, "T22: 0 data rows");

    /* Operations on empty frame should not crash. */
    uint64_t nr, nc;
    df_shape(r.ok, &nr, &nc);
    ASSERT(nr == 0 && nc == 3, "T22: shape is 0x3");

    TkDataframe *h = df_head(r.ok, 10);
    ASSERT(h != NULL && h->nrows == 0, "T22: head on empty frame returns 0 rows");
    df_free(h);

    const char *json = df_tojson(r.ok);
    ASSERT(json != NULL, "T22: tojson on empty frame non-NULL");
    ASSERT_STREQ(json, "[]", "T22: tojson on empty frame is '[]'");
    free((char *)json);

    const char *csv_out = df_tocsv(r.ok);
    ASSERT(csv_out != NULL, "T22: tocsv on empty frame non-NULL");
    free((char *)csv_out);

    df_free(r.ok);
}

static void t23_single_row_col(void)
{
    /* Single column, single row. */
    const char *csv = "x\r\n42\r\n";
    DfResult r = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!r.is_err,        "T23: single-cell CSV parses");
    ASSERT(r.ok != NULL,     "T23: frame non-NULL");
    ASSERT(r.ok->ncols == 1, "T23: 1 column");
    ASSERT(r.ok->nrows == 1, "T23: 1 row");
    DfCol *x = df_column(r.ok, "x");
    ASSERT(x != NULL && x->type == DF_COL_F64, "T23: x is f64");
    if (x) ASSERT(x->f64_data[0] == 42.0, "T23: x[0] == 42");
    df_free(r.ok);
}

static void t24_filter_nonexistent_col(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    TkDataframe *filtered = df_filter(df, "no_such_column", 0.0, 4);
    ASSERT(filtered == NULL, "T24: filter on nonexistent column returns NULL");
    df_free(df);
}

static void t25_filter_str_col(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    TkDataframe *filtered = df_filter(df, "name", 0.0, 4);
    ASSERT(filtered == NULL, "T25: filter on str column returns NULL");
    df_free(df);
}

static void t26_filter_no_matches(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    /* age > 1000 — nobody qualifies */
    TkDataframe *filtered = df_filter(df, "age", 1000.0, 4 /* > */);
    ASSERT(filtered != NULL,      "T26: filter returns non-NULL even with 0 matches");
    ASSERT(filtered->nrows == 0,  "T26: 0 rows match age > 1000");
    ASSERT(filtered->ncols == df->ncols, "T26: column count preserved");
    df_free(filtered);
    df_free(df);
}

static void t27_filter_equality(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    /* age == 30 → Alice and Carol */
    TkDataframe *filtered = df_filter(df, "age", 30.0, 2 /* == */);
    ASSERT(filtered != NULL,      "T27: filter == returns non-NULL");
    ASSERT(filtered->nrows == 2,  "T27: age == 30 yields 2 rows");
    df_free(filtered);
    df_free(df);
}

static void t28_join_missing_col(void)
{
    uint64_t ll = (uint64_t)strlen(CSV_LEFT);
    uint64_t rl = (uint64_t)strlen(CSV_RIGHT);
    DfResult lr = df_fromcsv(CSV_LEFT,  ll, 1);
    DfResult rr = df_fromcsv(CSV_RIGHT, rl, 1);
    if (lr.is_err || !lr.ok || rr.is_err || !rr.ok) {
        ASSERT(0, "T28: parsing join CSVs");
        df_free(lr.ok); df_free(rr.ok);
        return;
    }
    DfResult jr = df_join(lr.ok, rr.ok, "nonexistent_key");
    ASSERT(jr.is_err,      "T28: join on missing column returns error");
    ASSERT(jr.ok == NULL,  "T28: join on missing column has NULL ok");
    ASSERT(jr.err_msg != NULL, "T28: join error has message");
    df_free(lr.ok);
    df_free(rr.ok);
}

static void t29_groupby_mean(void)
{
    uint64_t len    = (uint64_t)strlen(CSV_GROUP);
    DfResult r      = df_fromcsv(CSV_GROUP, len, 1);
    if (r.is_err || !r.ok) { ASSERT(0, "T29: df_fromcsv group csv"); return; }
    TkDataframe *df = r.ok;
    DfGroupResult gr = df_groupby(df, "cat", "val", 2 /* mean */);
    ASSERT(gr.nrows == 3, "T29: groupby mean has 3 groups");

    /* Find group X: values 1,3 → mean 2.0 */
    int found_x = 0;
    for (uint64_t i = 0; i < gr.nrows; i++) {
        if (strcmp(gr.rows[i].group_val, "X") == 0) {
            ASSERT(gr.rows[i].agg_result == 2.0, "T29: mean(X) == 2.0");
            found_x = 1;
        }
    }
    ASSERT(found_x, "T29: group X found");

    for (uint64_t i = 0; i < gr.nrows; i++) free((char *)gr.rows[i].group_val);
    free(gr.rows);
    df_free(df);
}

static void t30_groupby_missing_cols(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;

    /* Group column doesn't exist */
    DfGroupResult gr1 = df_groupby(df, "nope", "age", 0);
    ASSERT(gr1.nrows == 0, "T30: groupby missing group_col returns 0 groups");
    ASSERT(gr1.rows == NULL, "T30: groupby missing group_col rows is NULL");

    /* Agg column doesn't exist */
    DfGroupResult gr2 = df_groupby(df, "name", "nope", 1);
    ASSERT(gr2.nrows == 0, "T30: groupby missing agg_col returns 0 groups");

    /* Group col is f64 (not str) — should fail */
    DfGroupResult gr3 = df_groupby(df, "age", "score", 0);
    ASSERT(gr3.nrows == 0, "T30: groupby on f64 group_col returns 0 groups");

    df_free(df);
}

static void t31_column_null_and_empty(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;

    DfCol *c1 = df_column(df, NULL);
    ASSERT(c1 == NULL, "T31: df_column(NULL name) returns NULL");

    DfCol *c2 = df_column(df, "");
    ASSERT(c2 == NULL, "T31: df_column(empty name) returns NULL");

    DfCol *c3 = df_column(NULL, "age");
    ASSERT(c3 == NULL, "T31: df_column(NULL df) returns NULL");

    df_free(df);
}

static void t32_duplicate_column_names(void)
{
    /* CSV with duplicate column name: two "x" columns. */
    const char *csv = "x,x\r\n1,hello\r\n";
    DfResult r = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!r.is_err, "T32: duplicate col name CSV parses");
    ASSERT(r.ok != NULL, "T32: frame non-NULL");
    if (r.ok) {
        ASSERT(r.ok->ncols == 2, "T32: 2 columns");
        /* df_column returns first match */
        DfCol *c = df_column(r.ok, "x");
        ASSERT(c != NULL, "T32: df_column finds 'x'");
        /* The first 'x' column has value "1" which is f64 */
        if (c) ASSERT(c->type == DF_COL_F64, "T32: first 'x' is f64 (value 1)");
    }
    df_free(r.ok);
}

static void t33_fromrows_zero_rows(void)
{
    const char *headers[] = {"a", "b"};
    TkDataframe *df = df_fromrows(headers, 2, NULL, 0);
    ASSERT(df != NULL,     "T33: fromrows with 0 rows returns non-NULL");
    if (df) {
        ASSERT(df->ncols == 2, "T33: 2 columns");
        ASSERT(df->nrows == 0, "T33: 0 rows");
    }
    df_free(df);
}

static void t34_fromrows_null_headers(void)
{
    TkDataframe *df = df_fromrows(NULL, 0, NULL, 0);
    ASSERT(df == NULL, "T34: fromrows with NULL headers returns NULL");
}

static void t35_tocsv_roundtrip(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;

    /* Serialize to CSV */
    const char *csv1 = df_tocsv(df);
    ASSERT(csv1 != NULL, "T35: first tocsv non-NULL");
    if (!csv1) { df_free(df); return; }

    /* Parse it back */
    DfResult r2 = df_fromcsv(csv1, (uint64_t)strlen(csv1), 1);
    ASSERT(!r2.is_err, "T35: round-trip parse succeeds");
    ASSERT(r2.ok != NULL, "T35: round-trip frame non-NULL");
    if (!r2.ok) { free((char *)csv1); df_free(df); return; }

    /* Same shape */
    ASSERT(r2.ok->ncols == df->ncols, "T35: round-trip ncols match");
    ASSERT(r2.ok->nrows == df->nrows, "T35: round-trip nrows match");

    /* Spot-check: name column still has Alice */
    DfCol *name = df_column(r2.ok, "name");
    ASSERT(name != NULL && name->type == DF_COL_STR, "T35: name col preserved");
    if (name && name->str_data) {
        ASSERT_STREQ(name->str_data[0], "Alice", "T35: name[0] still Alice");
    }

    /* Spot-check: age column still has 30 */
    DfCol *age = df_column(r2.ok, "age");
    ASSERT(age != NULL && age->type == DF_COL_F64, "T35: age col preserved as f64");
    if (age && age->f64_data) {
        ASSERT(age->f64_data[0] == 30.0, "T35: age[0] still 30");
    }

    df_free(r2.ok);
    free((char *)csv1);
    df_free(df);
}

static void t36_schema_mixed_types(void)
{
    /* Build a frame with str, f64, str columns */
    const char *headers[] = {"label", "count", "note"};
    const char *row0[] = {"a", "10", "ok"};
    const char *row1[] = {"b", "20", "fine"};
    const char **rows[] = {row0, row1};
    TkDataframe *df = df_fromrows(headers, 3, (const char ***)rows, 2);
    ASSERT(df != NULL, "T36: mixed-type frame created");
    if (!df) return;

    uint64_t len = 0;
    DfSeries *s = df_schema(df, &len);
    ASSERT(s != NULL, "T36: schema non-NULL");
    ASSERT(len == 3, "T36: 3 columns in schema");
    if (s && len == 3) {
        ASSERT_STREQ(s[0].dtype, "str", "T36: label is str");
        ASSERT_STREQ(s[1].dtype, "f64", "T36: count is f64");
        ASSERT_STREQ(s[2].dtype, "str", "T36: note is str");
        ASSERT(s[0].len == 2, "T36: label len == 2");
        ASSERT(s[1].len == 2, "T36: count len == 2");
    }
    if (s) {
        for (uint64_t i = 0; i < len; i++) { free(s[i].name); free(s[i].dtype); }
        free(s);
    }
    df_free(df);
}

static void t37_head_exceeds_nrows(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    TkDataframe *h = df_head(df, 100);
    ASSERT(h != NULL, "T37: head(100) on 4-row frame non-NULL");
    ASSERT(h->nrows == 4, "T37: head(100) returns all 4 rows");
    df_free(h);
    df_free(df);
}

static void t38_head_zero(void)
{
    TkDataframe *df = parse_hdr(CSV_WITH_HEADER);
    if (!df) return;
    TkDataframe *h = df_head(df, 0);
    ASSERT(h != NULL, "T38: head(0) returns non-NULL");
    ASSERT(h->nrows == 0, "T38: head(0) returns 0 rows");
    df_free(h);
    df_free(df);
}

static void t39_tojson_empty(void)
{
    TkDataframe *df = df_new();
    ASSERT(df != NULL, "T39: df_new non-NULL");
    const char *json = df_tojson(df);
    ASSERT(json != NULL, "T39: tojson on brand-new frame non-NULL");
    ASSERT_STREQ(json, "[]", "T39: tojson on empty df_new is '[]'");
    free((char *)json);
    df_free(df);
}

static void t40_shape_null(void)
{
    uint64_t nr = 999, nc = 999;
    df_shape(NULL, &nr, &nc);
    ASSERT(nr == 0, "T40: shape(NULL) sets nrows to 0");
    ASSERT(nc == 0, "T40: shape(NULL) sets ncols to 0");
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
    t16_fromrows();
    t17_columnstr();
    t18_columnstr_f64_returns_null();
    t19_tocsv();
    t20_schema();
    t21_large_dataframe();
    t22_empty_dataframe();
    t23_single_row_col();
    t24_filter_nonexistent_col();
    t25_filter_str_col();
    t26_filter_no_matches();
    t27_filter_equality();
    t28_join_missing_col();
    t29_groupby_mean();
    t30_groupby_missing_cols();
    t31_column_null_and_empty();
    t32_duplicate_column_names();
    t33_fromrows_zero_rows();
    t34_fromrows_null_headers();
    t35_tocsv_roundtrip();
    t36_schema_mixed_types();
    t37_head_exceeds_nrows();
    t38_head_zero();
    t39_tojson_empty();
    t40_shape_null();

    printf("\n%s — %d failure(s)\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
