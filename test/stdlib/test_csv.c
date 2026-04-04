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

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
