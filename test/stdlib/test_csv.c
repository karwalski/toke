/*
 * test_csv.c — Unit tests for the std.csv C library (Story 16.1.1).
 *
 * Build and run: make test-stdlib-csv
 *
 * Covers:
 *   - Simple 2-col 2-row parsing
 *   - Quoted field with embedded comma
 *   - Quoted field with embedded newline (round-trip)
 *   - Doubled-quote escape (say ""hi"" → say "hi")
 *   - Empty fields
 *   - CRLF line endings
 *   - Writer round-trip
 *   - header() returns first row without advancing past it
 *   - has_next() false at EOF
 *   - csv_parse nrows_out
 *   - csv_parse single row
 *   - csv_parse empty string returns NULL
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../src/stdlib/csv.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

#define ASSERT_INTEQ(a, b, msg) \
    ASSERT((a) == (b), msg)

/* Free all strings in a StrArray and the data pointer array itself. */
static void free_row(StrArray row)
{
    for (uint64_t i = 0; i < row.len; i++)
        free((void *)row.data[i]);
    free((void *)row.data);
}

/* Free an array of StrArrays returned by csv_parse. */
static void free_rows(StrArray *rows, uint64_t nrows)
{
    for (uint64_t i = 0; i < nrows; i++) free_row(rows[i]);
    free(rows);
}

/* -----------------------------------------------------------------------
 * Test 1: Simple 2-col 2-row parse ("a,b\nc,d")
 * ----------------------------------------------------------------------- */
static void test_simple_2col_2row(void)
{
    const char *csv = "a,b\nc,d";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));

    ASSERT(csv_reader_has_next(r), "simple: has_next before first row");
    StrArray row0 = csv_reader_next(r);
    ASSERT_INTEQ((int)row0.len, 2, "simple: row0 has 2 fields");
    ASSERT_STREQ(row0.data[0], "a", "simple: row0[0]=='a'");
    ASSERT_STREQ(row0.data[1], "b", "simple: row0[1]=='b'");

    ASSERT(csv_reader_has_next(r), "simple: has_next before second row");
    StrArray row1 = csv_reader_next(r);
    ASSERT_INTEQ((int)row1.len, 2, "simple: row1 has 2 fields");
    ASSERT_STREQ(row1.data[0], "c", "simple: row1[0]=='c'");
    ASSERT_STREQ(row1.data[1], "d", "simple: row1[1]=='d'");

    ASSERT(!csv_reader_has_next(r), "simple: has_next false at EOF");

    free_row(row0);
    free_row(row1);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 2: Quoted field with embedded comma ("hello, world",b)
 * ----------------------------------------------------------------------- */
static void test_quoted_comma(void)
{
    const char *csv = "\"hello, world\",b";
    TkCsvReader *r  = csv_reader_new(csv, (uint64_t)strlen(csv));

    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 2, "quoted_comma: row has 2 fields");
    ASSERT_STREQ(row.data[0], "hello, world", "quoted_comma: field0");
    ASSERT_STREQ(row.data[1], "b",            "quoted_comma: field1");

    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 3: Quoted field with embedded newline (round-trip)
 * ----------------------------------------------------------------------- */
static void test_quoted_newline_roundtrip(void)
{
    /* Build the original StrArray manually and write it. */
    const char *fields_raw[2] = {"line1\nline2", "end"};
    StrArray original;
    original.data = fields_raw;
    original.len  = 2;

    TkCsvWriter *w = csv_writer_new();
    csv_writer_writerow(w, original);
    const char *serialised = csv_writer_flush(w);

    /* Parse back */
    TkCsvReader *r = csv_reader_new(serialised, (uint64_t)strlen(serialised));
    StrArray row   = csv_reader_next(r);

    ASSERT_INTEQ((int)row.len, 2,               "embedded_nl: round-trip field count");
    ASSERT_STREQ(row.data[0], "line1\nline2",    "embedded_nl: field0 preserved");
    ASSERT_STREQ(row.data[1], "end",             "embedded_nl: field1 preserved");

    free_row(row);
    csv_reader_free(r);
    csv_writer_free(w);
    free((void *)serialised);
}

/* -----------------------------------------------------------------------
 * Test 4: Doubled-quote escape — "say ""hi""" → say "hi"
 * ----------------------------------------------------------------------- */
static void test_doubled_quote(void)
{
    /* CSV text (no outer quotes around the whole thing):  "say ""hi"""  */
    const char *csv = "\"say \\\"\\\"hi\\\"\\\"\"";
    /* Actually build it as a raw C string without backslash confusion: */
    const char raw[] = {'"','s','a','y',' ','"','"','h','i','"','"','"','\0'};
    TkCsvReader *r = csv_reader_new(raw, (uint64_t)(sizeof(raw)-1));
    StrArray row   = csv_reader_next(r);

    ASSERT_INTEQ((int)row.len, 1,          "doubled_quote: one field");
    ASSERT_STREQ(row.data[0], "say \"hi\"", "doubled_quote: value");

    (void)csv; /* suppress unused-variable warning */
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 5: Empty fields  (",," → ["","",""])
 * ----------------------------------------------------------------------- */
static void test_empty_fields(void)
{
    const char *csv = ",,";
    TkCsvReader *r  = csv_reader_new(csv, (uint64_t)strlen(csv));

    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 3,  "empty_fields: 3 fields");
    ASSERT_STREQ(row.data[0], "", "empty_fields: field0 empty");
    ASSERT_STREQ(row.data[1], "", "empty_fields: field1 empty");
    ASSERT_STREQ(row.data[2], "", "empty_fields: field2 empty");

    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 6: CRLF line endings ("a\r\nb" → two rows)
 * ----------------------------------------------------------------------- */
static void test_crlf(void)
{
    const char *csv = "a\r\nb";
    TkCsvReader *r  = csv_reader_new(csv, (uint64_t)strlen(csv));

    StrArray row0 = csv_reader_next(r);
    StrArray row1 = csv_reader_next(r);

    ASSERT_INTEQ((int)row0.len, 1, "crlf: row0 has 1 field");
    ASSERT_STREQ(row0.data[0], "a", "crlf: row0[0]=='a'");
    ASSERT_INTEQ((int)row1.len, 1, "crlf: row1 has 1 field");
    ASSERT_STREQ(row1.data[0], "b", "crlf: row1[0]=='b'");
    ASSERT(!csv_reader_has_next(r),  "crlf: EOF after two rows");

    free_row(row0);
    free_row(row1);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 7: Writer round-trip — write multi-row then parse equals original
 * ----------------------------------------------------------------------- */
static void test_writer_roundtrip(void)
{
    const char *r0[3] = {"name",  "age", "city"};
    const char *r1[3] = {"Alice", "30",  "New York"};
    const char *r2[3] = {"Bob",   "25",  "San Francisco, CA"};

    StrArray row0 = {r0, 3};
    StrArray row1 = {r1, 3};
    StrArray row2 = {r2, 3};

    TkCsvWriter *w = csv_writer_new();
    csv_writer_writerow(w, row0);
    csv_writer_writerow(w, row1);
    csv_writer_writerow(w, row2);
    const char *out = csv_writer_flush(w);

    uint64_t nrows = 0;
    StrArray *rows = csv_parse(out, (uint64_t)strlen(out), &nrows);

    ASSERT_INTEQ((int)nrows, 3,                       "roundtrip: 3 rows");
    ASSERT_STREQ(rows[0].data[0], "name",             "roundtrip: [0][0]");
    ASSERT_STREQ(rows[1].data[0], "Alice",            "roundtrip: [1][0]");
    ASSERT_STREQ(rows[2].data[2], "San Francisco, CA","roundtrip: [2][2] comma-field");

    free_rows(rows, nrows);
    csv_writer_free(w);
    free((void *)out);
}

/* -----------------------------------------------------------------------
 * Test 8: header() returns first row without advancing past it
 * ----------------------------------------------------------------------- */
static void test_header(void)
{
    const char *csv = "id,name\n1,Alice\n2,Bob";
    TkCsvReader *r  = csv_reader_new(csv, (uint64_t)strlen(csv));

    /* has_next was called zero times; header() should read and cache row 0. */
    StrArray hdr = csv_reader_header(r);
    ASSERT_INTEQ((int)hdr.len, 2,      "header: two header fields");
    ASSERT_STREQ(hdr.data[0], "id",   "header: field0=='id'");
    ASSERT_STREQ(hdr.data[1], "name", "header: field1=='name'");

    /* Calling header() again should return the same cached values. */
    StrArray hdr2 = csv_reader_header(r);
    ASSERT_STREQ(hdr2.data[0], "id",  "header: cached call still returns 'id'");

    /* next() should now return the data rows, not the header again. */
    StrArray data0 = csv_reader_next(r);
    ASSERT_STREQ(data0.data[0], "1",     "header: data row 0 field 0");
    ASSERT_STREQ(data0.data[1], "Alice", "header: data row 0 field 1");
    free_row(data0);

    StrArray data1 = csv_reader_next(r);
    ASSERT_STREQ(data1.data[0], "2",   "header: data row 1 field 0");
    ASSERT_STREQ(data1.data[1], "Bob", "header: data row 1 field 1");
    free_row(data1);

    ASSERT(!csv_reader_has_next(r), "header: EOF after data rows");

    /* hdr / hdr2 are owned by the reader, freed in csv_reader_free. */
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 9: has_next() false at EOF immediately on empty input
 * ----------------------------------------------------------------------- */
static void test_has_next_empty(void)
{
    TkCsvReader *r = csv_reader_new("", 0);
    ASSERT(!csv_reader_has_next(r), "has_next_empty: false on empty input");
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 10: csv_parse nrows_out correct
 * ----------------------------------------------------------------------- */
static void test_csv_parse_nrows(void)
{
    const char *csv = "a,b\nc,d\ne,f";
    uint64_t nrows  = 0;
    StrArray *rows  = csv_parse(csv, (uint64_t)strlen(csv), &nrows);

    ASSERT_INTEQ((int)nrows, 3, "csv_parse_nrows: 3 rows");
    ASSERT_STREQ(rows[2].data[0], "e", "csv_parse_nrows: last row field0=='e'");

    free_rows(rows, nrows);
}

/* -----------------------------------------------------------------------
 * Test 11: csv_parse single row
 * ----------------------------------------------------------------------- */
static void test_csv_parse_single_row(void)
{
    const char *csv = "x,y,z";
    uint64_t nrows  = 0;
    StrArray *rows  = csv_parse(csv, (uint64_t)strlen(csv), &nrows);

    ASSERT_INTEQ((int)nrows, 1,   "csv_parse_single: 1 row");
    ASSERT_INTEQ((int)rows[0].len, 3, "csv_parse_single: 3 fields");
    ASSERT_STREQ(rows[0].data[2], "z", "csv_parse_single: field2=='z'");

    free_rows(rows, nrows);
}

/* -----------------------------------------------------------------------
 * Test 12: csv_parse empty string returns NULL, nrows_out==0
 * ----------------------------------------------------------------------- */
static void test_csv_parse_empty(void)
{
    uint64_t nrows = 99;
    StrArray *rows = csv_parse("", 0, &nrows);
    ASSERT(rows == NULL,         "csv_parse_empty: returns NULL");
    ASSERT_INTEQ((int)nrows, 0, "csv_parse_empty: nrows_out==0");
}

/* -----------------------------------------------------------------------
 * Test 13: Field containing only quotes — """" → single "
 * ----------------------------------------------------------------------- */
static void test_field_only_quotes(void)
{
    /* CSV: """""" which is quoted field containing "" → one literal " */
    const char raw[] = {'"','"','"','"','"','"','\0'};
    TkCsvReader *r = csv_reader_new(raw, (uint64_t)(sizeof(raw) - 1));
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 1,         "only_quotes: 1 field");
    ASSERT_STREQ(row.data[0], "\"\"",      "only_quotes: value is two quotes");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 14: Field containing only newlines — quoted "\n\n"
 * ----------------------------------------------------------------------- */
static void test_field_only_newlines(void)
{
    const char csv[] = "\"\n\n\"";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)(sizeof(csv) - 1));
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 1,           "only_newlines: 1 field");
    ASSERT_STREQ(row.data[0], "\n\n",       "only_newlines: value is two newlines");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 15: Zero-column row — empty string before newline yields 1 empty field
 * ----------------------------------------------------------------------- */
static void test_zero_column_row(void)
{
    /* A line with no commas produces one (empty) field. */
    const char *csv = "\n";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 1,  "zero_col: 1 field");
    ASSERT_STREQ(row.data[0], "",  "zero_col: field is empty");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 16: Many-column row (100+ columns)
 * ----------------------------------------------------------------------- */
static void test_many_columns(void)
{
    /* Build a CSV row with 150 columns: "0,1,2,...,149" */
    char buf[1024];
    int offset = 0;
    for (int i = 0; i < 150; i++) {
        if (i > 0) buf[offset++] = ',';
        offset += sprintf(buf + offset, "%d", i);
    }
    TkCsvReader *r = csv_reader_new(buf, (uint64_t)offset);
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 150,   "many_cols: 150 fields");
    ASSERT_STREQ(row.data[0], "0",    "many_cols: first field");
    ASSERT_STREQ(row.data[149], "149", "many_cols: last field");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 17: Large streaming read (100KB+)
 * ----------------------------------------------------------------------- */
static void test_large_streaming(void)
{
    /* Build a CSV buffer > 100KB: many rows of "aaa,bbb,ccc\n" (12 bytes each) */
    uint64_t target = 102400;
    const char *line = "aaa,bbb,ccc\n";
    uint64_t linelen = (uint64_t)strlen(line);
    uint64_t nlines = (target / linelen) + 1;
    uint64_t buflen = nlines * linelen;
    char *buf = (char *)malloc(buflen);
    for (uint64_t i = 0; i < nlines; i++)
        memcpy(buf + i * linelen, line, linelen);

    ASSERT(buflen >= 102400, "large_stream: buffer >= 100KB");

    TkCsvReader *r = csv_reader_new(buf, buflen);
    uint64_t count = 0;
    while (csv_reader_has_next(r)) {
        StrArray row = csv_reader_next(r);
        ASSERT_INTEQ((int)row.len, 3, "large_stream: each row has 3 fields");
        free_row(row);
        count++;
        if ((int)row.len != 3) break; /* stop on unexpected to avoid spam */
    }
    ASSERT_INTEQ((int)count, (int)nlines, "large_stream: correct row count");

    csv_reader_free(r);
    free(buf);
}

/* -----------------------------------------------------------------------
 * Test 18: UTF-8 BOM handling — BOM bytes appear in first field
 * (RFC 4180 does not define BOM handling; we verify the parser does not
 *  crash and the bytes are preserved in the field value.)
 * ----------------------------------------------------------------------- */
static void test_bom_handling(void)
{
    /* UTF-8 BOM: EF BB BF, then "a,b" */
    const char csv[] = "\xEF\xBB\xBF" "a,b";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)(sizeof(csv) - 1));
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 2,                    "bom: 2 fields");
    /* BOM bytes are part of the first field value */
    ASSERT_STREQ(row.data[0], "\xEF\xBB\xBF" "a",   "bom: field0 contains BOM + 'a'");
    ASSERT_STREQ(row.data[1], "b",                    "bom: field1 == 'b'");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 19: Trailing CRLF variations
 * ----------------------------------------------------------------------- */
static void test_trailing_crlf_variations(void)
{
    /* CR only: "a\r" → one row with field "a", then EOF */
    {
        const char *csv = "a\r";
        TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
        StrArray row = csv_reader_next(r);
        ASSERT_INTEQ((int)row.len, 1,   "trailing_cr: 1 field");
        ASSERT_STREQ(row.data[0], "a",  "trailing_cr: field == 'a'");
        ASSERT(!csv_reader_has_next(r),  "trailing_cr: EOF");
        free_row(row);
        csv_reader_free(r);
    }
    /* LF only: "a\n" */
    {
        const char *csv = "a\n";
        TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
        StrArray row = csv_reader_next(r);
        ASSERT_INTEQ((int)row.len, 1,   "trailing_lf: 1 field");
        ASSERT_STREQ(row.data[0], "a",  "trailing_lf: field == 'a'");
        ASSERT(!csv_reader_has_next(r),  "trailing_lf: EOF");
        free_row(row);
        csv_reader_free(r);
    }
    /* CRLF: "a\r\n" */
    {
        const char *csv = "a\r\n";
        TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
        StrArray row = csv_reader_next(r);
        ASSERT_INTEQ((int)row.len, 1,     "trailing_crlf: 1 field");
        ASSERT_STREQ(row.data[0], "a",    "trailing_crlf: field == 'a'");
        ASSERT(!csv_reader_has_next(r),    "trailing_crlf: EOF");
        free_row(row);
        csv_reader_free(r);
    }
    /* No trailing newline: "a" */
    {
        const char *csv = "a";
        TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
        StrArray row = csv_reader_next(r);
        ASSERT_INTEQ((int)row.len, 1,     "trailing_none: 1 field");
        ASSERT_STREQ(row.data[0], "a",    "trailing_none: field == 'a'");
        ASSERT(!csv_reader_has_next(r),    "trailing_none: EOF");
        free_row(row);
        csv_reader_free(r);
    }
}

/* -----------------------------------------------------------------------
 * Test 20: Empty fields in various positions
 * ----------------------------------------------------------------------- */
static void test_empty_fields_positions(void)
{
    /* Leading empty, middle value, trailing empty: ",val," */
    const char *csv = ",val,";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 3,        "empty_pos: 3 fields");
    ASSERT_STREQ(row.data[0], "",         "empty_pos: leading empty");
    ASSERT_STREQ(row.data[1], "val",      "empty_pos: middle value");
    ASSERT_STREQ(row.data[2], "",         "empty_pos: trailing empty");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 21: Fields with embedded commas (multiple)
 * ----------------------------------------------------------------------- */
static void test_embedded_commas(void)
{
    /* "a,b,c" as a single quoted field, then plain d */
    const char *csv = "\"a,b,c\",d";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 2,           "embed_comma: 2 fields");
    ASSERT_STREQ(row.data[0], "a,b,c",      "embed_comma: field0 has commas");
    ASSERT_STREQ(row.data[1], "d",           "embed_comma: field1 == 'd'");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 22: Fields with embedded escaped quotes
 * ----------------------------------------------------------------------- */
static void test_embedded_escaped_quotes(void)
{
    /* CSV: "she said ""hello""",ok */
    const char raw[] = {'"','s','h','e',' ','s','a','i','d',' ',
                        '"','"','h','e','l','l','o','"','"','"',
                        ',','o','k','\0'};
    TkCsvReader *r = csv_reader_new(raw, (uint64_t)(sizeof(raw) - 1));
    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 2,                          "embed_esc_q: 2 fields");
    ASSERT_STREQ(row.data[0], "she said \"hello\"",        "embed_esc_q: field0");
    ASSERT_STREQ(row.data[1], "ok",                        "embed_esc_q: field1");
    free_row(row);
    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * Test 23: Full round-trip — write then read returns original data
 * ----------------------------------------------------------------------- */
static void test_full_roundtrip(void)
{
    /* Tricky fields: empty, has comma, has quote, has newline, has CR */
    const char *fields[5] = {"", "a,b", "say \"hi\"", "line1\nline2", "cr\rhere"};
    StrArray row_in;
    row_in.data = fields;
    row_in.len  = 5;

    TkCsvWriter *w = csv_writer_new();
    csv_writer_writerow(w, row_in);
    const char *out = csv_writer_flush(w);

    uint64_t nrows = 0;
    StrArray *rows = csv_parse(out, (uint64_t)strlen(out), &nrows);
    ASSERT_INTEQ((int)nrows, 1,                          "full_rt: 1 row");
    ASSERT_INTEQ((int)rows[0].len, 5,                    "full_rt: 5 fields");
    ASSERT_STREQ(rows[0].data[0], "",                     "full_rt: empty field");
    ASSERT_STREQ(rows[0].data[1], "a,b",                  "full_rt: comma field");
    ASSERT_STREQ(rows[0].data[2], "say \"hi\"",           "full_rt: quote field");
    ASSERT_STREQ(rows[0].data[3], "line1\nline2",         "full_rt: newline field");
    ASSERT_STREQ(rows[0].data[4], "cr\rhere",             "full_rt: CR field");

    free_rows(rows, nrows);
    csv_writer_free(w);
    free((void *)out);
}

/* -----------------------------------------------------------------------
 * Test 24: Multi-row round-trip preserves all data
 * ----------------------------------------------------------------------- */
static void test_multirow_roundtrip(void)
{
    const char *h[2]  = {"key", "value"};
    const char *r1[2] = {"greeting", "hello, world"};
    const char *r2[2] = {"quote", "she said \"yes\""};
    const char *r3[2] = {"multi", "line1\nline2\nline3"};

    StrArray rows_in[4] = {
        {h,  2}, {r1, 2}, {r2, 2}, {r3, 2}
    };

    TkCsvWriter *w = csv_writer_new();
    for (int i = 0; i < 4; i++) csv_writer_writerow(w, rows_in[i]);
    const char *out = csv_writer_flush(w);

    uint64_t nrows = 0;
    StrArray *rows = csv_parse(out, (uint64_t)strlen(out), &nrows);
    ASSERT_INTEQ((int)nrows, 4,                             "multi_rt: 4 rows");
    ASSERT_STREQ(rows[0].data[0], "key",                    "multi_rt: header[0]");
    ASSERT_STREQ(rows[1].data[1], "hello, world",           "multi_rt: row1 comma");
    ASSERT_STREQ(rows[2].data[1], "she said \"yes\"",       "multi_rt: row2 quote");
    ASSERT_STREQ(rows[3].data[1], "line1\nline2\nline3",    "multi_rt: row3 newlines");

    free_rows(rows, nrows);
    csv_writer_free(w);
    free((void *)out);
}

/* -----------------------------------------------------------------------
 * Story 29.4.1 tests — configuration and dialects
 * ----------------------------------------------------------------------- */

/* Test 25: TSV — separator '\t', parse "a\tb\tc\n" → 3 fields */
static void test_tsv_separator(void)
{
    const char *tsv = "a\tb\tc\n";
    TkCsvReader *r = csv_reader_new(tsv, (uint64_t)strlen(tsv));
    csv_reader_set_separator(r, '\t');

    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 3,   "tsv: 3 fields");
    ASSERT_STREQ(row.data[0], "a",  "tsv: field0 == 'a'");
    ASSERT_STREQ(row.data[1], "b",  "tsv: field1 == 'b'");
    ASSERT_STREQ(row.data[2], "c",  "tsv: field2 == 'c'");

    free_row(row);
    csv_reader_free(r);
}

/* Test 26: Custom quote character '|' — |field with "quotes"| → correct field */
static void test_custom_quote_char(void)
{
    const char *csv = "|field with \"quotes\"|";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));
    csv_reader_set_quote(r, '|');

    StrArray row = csv_reader_next(r);
    ASSERT_INTEQ((int)row.len, 1,                       "custom_quote: 1 field");
    ASSERT_STREQ(row.data[0], "field with \"quotes\"",  "custom_quote: field value");

    free_row(row);
    csv_reader_free(r);
}

/* Test 27: Writer separator '\t' — output contains tabs, not commas */
static void test_writer_tsv_separator(void)
{
    const char *fields[3] = {"x", "y", "z"};
    StrArray row;
    row.data = fields;
    row.len  = 3;

    TkCsvWriter *w = csv_writer_new();
    csv_writer_set_separator(w, '\t');
    csv_writer_writerow(w, row);
    const char *out = csv_writer_flush(w);

    /* Verify tabs present and commas absent between fields */
    ASSERT(strchr(out, '\t') != NULL,  "writer_tsv: output has tabs");
    /* The only commas that could appear would be inside fields (none here) */
    ASSERT(out[1] == '\t',             "writer_tsv: second char is tab");
    ASSERT(out[3] == '\t',             "writer_tsv: fourth char is tab");

    csv_writer_free(w);
    free((void *)out);
}

/* Test 28: Writer use_crlf(1) — row ends with \r\n */
static void test_writer_crlf(void)
{
    const char *fields[2] = {"a", "b"};
    StrArray row;
    row.data = fields;
    row.len  = 2;

    /* use_crlf already defaults to 1, but call it explicitly */
    TkCsvWriter *w = csv_writer_new();
    csv_writer_use_crlf(w, 1);
    csv_writer_writerow(w, row);
    /* Write a second row so the \r\n row terminator is actually emitted */
    csv_writer_writerow(w, row);
    const char *out = csv_writer_flush(w);
    uint64_t outlen = (uint64_t)strlen(out);

    /* First row ends with \r\n (before the second row begins) */
    ASSERT(outlen >= 4,                    "writer_crlf: output has content");
    /* Find the \r\n separator between row 0 and row 1 */
    int found_crlf = 0;
    for (uint64_t i = 0; i + 1 < outlen; i++) {
        if (out[i] == '\r' && out[i + 1] == '\n') { found_crlf = 1; break; }
    }
    ASSERT(found_crlf, "writer_crlf: \\r\\n present between rows");

    csv_writer_free(w);
    free((void *)out);
}

/* Test 29: Writer use_crlf(0) — row ends with \n, no \r */
static void test_writer_lf_only(void)
{
    const char *fields[2] = {"a", "b"};
    StrArray row;
    row.data = fields;
    row.len  = 2;

    TkCsvWriter *w = csv_writer_new();
    csv_writer_use_crlf(w, 0);
    csv_writer_writerow(w, row);
    csv_writer_writerow(w, row);
    const char *out = csv_writer_flush(w);

    /* There must be a \n separator */
    ASSERT(strchr(out, '\n') != NULL,   "writer_lf: \\n present");
    /* And no \r character at all */
    ASSERT(strchr(out, '\r') == NULL,   "writer_lf: no \\r present");

    csv_writer_free(w);
    free((void *)out);
}

/* Test 30: csv_reader_line_number — parse 3 rows, verify increments */
static void test_line_number(void)
{
    const char *csv = "a,b\nc,d\ne,f";
    TkCsvReader *r = csv_reader_new(csv, (uint64_t)strlen(csv));

    ASSERT_INTEQ((int)csv_reader_line_number(r), 0, "line_number: 0 before any read");

    StrArray row0 = csv_reader_next(r);
    ASSERT_INTEQ((int)csv_reader_line_number(r), 1, "line_number: 1 after row 1");
    free_row(row0);

    StrArray row1 = csv_reader_next(r);
    ASSERT_INTEQ((int)csv_reader_line_number(r), 2, "line_number: 2 after row 2");
    free_row(row1);

    StrArray row2 = csv_reader_next(r);
    ASSERT_INTEQ((int)csv_reader_line_number(r), 3, "line_number: 3 after row 3");
    free_row(row2);

    csv_reader_free(r);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    test_simple_2col_2row();
    test_quoted_comma();
    test_quoted_newline_roundtrip();
    test_doubled_quote();
    test_empty_fields();
    test_crlf();
    test_writer_roundtrip();
    test_header();
    test_has_next_empty();
    test_csv_parse_nrows();
    test_csv_parse_single_row();
    test_csv_parse_empty();
    test_field_only_quotes();
    test_field_only_newlines();
    test_zero_column_row();
    test_many_columns();
    test_large_streaming();
    test_bom_handling();
    test_trailing_crlf_variations();
    test_empty_fields_positions();
    test_embedded_commas();
    test_embedded_escaped_quotes();
    test_full_roundtrip();
    test_multirow_roundtrip();
    test_tsv_separator();
    test_custom_quote_char();
    test_writer_tsv_separator();
    test_writer_crlf();
    test_writer_lf_only();
    test_line_number();

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
