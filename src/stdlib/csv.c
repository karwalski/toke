/*
 * csv.c — Implementation of the std.csv standard library module.
 *
 * Implements RFC 4180 CSV parsing and serialisation:
 *   - Fields separated by ',' ; records terminated by CRLF or LF.
 *   - Quoted fields: "..."; doubled "" inside = literal ".
 *   - Fields may contain commas, newlines, and quotes when quoted.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own all returned strings and arrays.
 *
 * Story: 16.1.1
 */

#include "csv.h"
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* Growable byte buffer used by both the reader (field accumulation) and the
 * writer (output accumulation). */
typedef struct {
    char    *data;
    uint64_t len;
    uint64_t cap;
} GrowBuf;

static void gbuf_init(GrowBuf *b)
{
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void gbuf_free(GrowBuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

static void gbuf_ensure(GrowBuf *b, uint64_t extra)
{
    uint64_t need = b->len + extra;
    if (need <= b->cap) return;
    uint64_t newcap = b->cap ? b->cap * 2 : 64;
    while (newcap < need) newcap *= 2;
    b->data = (char *)realloc(b->data, newcap);
    b->cap  = newcap;
}

static void gbuf_push(GrowBuf *b, char c)
{
    gbuf_ensure(b, 1);
    b->data[b->len++] = c;
}

static void gbuf_append(GrowBuf *b, const char *s, uint64_t n)
{
    gbuf_ensure(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

/* Return a heap-allocated NUL-terminated copy of b's contents. */
static char *gbuf_to_str(GrowBuf *b)
{
    char *s = (char *)malloc(b->len + 1);
    if (b->len) memcpy(s, b->data, b->len);
    s[b->len] = '\0';
    return s;
}

/* -----------------------------------------------------------------------
 * Row (field list) accumulation
 * ----------------------------------------------------------------------- */

typedef struct {
    char    **fields;
    uint64_t  len;
    uint64_t  cap;
} RowBuf;

static void rowbuf_init(RowBuf *r)
{
    r->fields = NULL;
    r->len    = 0;
    r->cap    = 0;
}

static void rowbuf_push(RowBuf *r, char *field)
{
    if (r->len == r->cap) {
        uint64_t newcap = r->cap ? r->cap * 2 : 8;
        r->fields = (char **)realloc(r->fields, newcap * sizeof(char *));
        r->cap    = newcap;
    }
    r->fields[r->len++] = field;
}

/* Transfer the accumulated fields into a heap-allocated StrArray.
 * The RowBuf's internal pointer array is donated to the StrArray (no copy). */
static StrArray rowbuf_to_strarray(RowBuf *r)
{
    StrArray sa;
    sa.data = (const char **)r->fields;
    sa.len  = r->len;
    r->fields = NULL;
    r->len    = 0;
    r->cap    = 0;
    return sa;
}

static void __attribute__((unused)) rowbuf_free_fields(RowBuf *r)
{
    for (uint64_t i = 0; i < r->len; i++) free(r->fields[i]);
    free(r->fields);
    r->fields = NULL;
    r->len    = 0;
    r->cap    = 0;
}

/* -----------------------------------------------------------------------
 * Low-level RFC 4180 parser
 *
 * parse_row() reads one row from (data+pos, len) and advances *pos.
 * Returns 1 if a row was parsed, 0 at EOF (before reading any byte).
 * On return *row contains the fields (caller owns them).
 *
 * sep        — field separator character
 * quote_char — quote character
 * lazyquotes — if non-zero, a bare quote in an unquoted field is literal
 * ----------------------------------------------------------------------- */

static int parse_row(const char *data, uint64_t len, uint64_t *pos,
                     RowBuf *row, char sep, char quote_char, int lazyquotes)
{
    if (*pos >= len) return 0;

    GrowBuf field;
    gbuf_init(&field);
    rowbuf_init(row);

    int in_quotes = 0;

    while (*pos < len) {
        char c = data[*pos];

        if (in_quotes) {
            if (c == quote_char) {
                /* Peek at next char to distinguish doubled-quote from close */
                if (*pos + 1 < len && data[*pos + 1] == quote_char) {
                    /* Escaped quote: qq → q */
                    gbuf_push(&field, quote_char);
                    *pos += 2;
                } else {
                    /* Closing quote */
                    in_quotes = 0;
                    (*pos)++;
                }
            } else {
                gbuf_push(&field, c);
                (*pos)++;
            }
        } else {
            if (c == quote_char) {
                /* In lazyquotes mode, a quote that appears after the field
                 * has already accumulated characters is treated as a literal
                 * rather than opening a quoted section. */
                if (lazyquotes && field.len > 0) {
                    gbuf_push(&field, c);
                    (*pos)++;
                } else {
                    in_quotes = 1;
                    (*pos)++;
                }
            } else if (c == sep) {
                rowbuf_push(row, gbuf_to_str(&field));
                field.len = 0; /* reset without freeing buffer */
                (*pos)++;
            } else if (c == '\r') {
                /* CRLF or bare CR: consume and end row */
                (*pos)++;
                if (*pos < len && data[*pos] == '\n') (*pos)++;
                rowbuf_push(row, gbuf_to_str(&field));
                gbuf_free(&field);
                return 1;
            } else if (c == '\n') {
                (*pos)++;
                rowbuf_push(row, gbuf_to_str(&field));
                gbuf_free(&field);
                return 1;
            } else {
                gbuf_push(&field, c);
                (*pos)++;
            }
        }
    }

    /* EOF: flush last field */
    rowbuf_push(row, gbuf_to_str(&field));
    gbuf_free(&field);
    return 1;
}

/* -----------------------------------------------------------------------
 * TkCsvReader
 * ----------------------------------------------------------------------- */

struct TkCsvReader {
    const char *data;
    uint64_t    len;
    uint64_t    pos;

    int         header_read;   /* 1 if header row has been fetched */
    StrArray    header_cache;  /* cached first row */

    /* Dialect settings (Story 29.4.1) */
    char        sep;           /* field separator, default ',' */
    char        quote_char;    /* quote character, default '"' */
    int         lazyquotes;    /* lenient bare-quote handling */
    uint64_t    line_number;   /* 1-based; 0 before any row read */
};

TkCsvReader *csv_reader_new(const char *data, uint64_t len)
{
    TkCsvReader *r = (TkCsvReader *)malloc(sizeof(TkCsvReader));
    r->data         = data;
    r->len          = len;
    r->pos          = 0;
    r->header_read  = 0;
    r->header_cache.data = NULL;
    r->header_cache.len  = 0;
    r->sep          = ',';
    r->quote_char   = '"';
    r->lazyquotes   = 0;
    r->line_number  = 0;
    return r;
}

void csv_reader_set_separator(TkCsvReader *r, char sep)
{
    r->sep = sep;
}

void csv_reader_set_quote(TkCsvReader *r, char ch)
{
    r->quote_char = ch;
}

void csv_reader_lazyquotes(TkCsvReader *r, int enabled)
{
    r->lazyquotes = enabled;
}

uint64_t csv_reader_line_number(TkCsvReader *r)
{
    return r->line_number;
}

void csv_reader_free(TkCsvReader *r)
{
    if (!r) return;
    if (r->header_read && r->header_cache.data) {
        for (uint64_t i = 0; i < r->header_cache.len; i++)
            free((void *)r->header_cache.data[i]);
        free((void *)r->header_cache.data);
    }
    free(r);
}

int csv_reader_has_next(TkCsvReader *r)
{
    return r->pos < r->len;
}

StrArray csv_reader_next(TkCsvReader *r)
{
    StrArray empty = {NULL, 0};
    if (r->pos >= r->len) return empty;

    RowBuf row;
    if (!parse_row(r->data, r->len, &r->pos, &row,
                   r->sep, r->quote_char, r->lazyquotes)) return empty;
    r->line_number++;
    return rowbuf_to_strarray(&row);
}

StrArray csv_reader_header(TkCsvReader *r)
{
    if (r->header_read) return r->header_cache;

    /* Read the first row and cache it. */
    StrArray row = csv_reader_next(r);
    r->header_cache = row;
    r->header_read  = 1;
    return r->header_cache;
}

/* -----------------------------------------------------------------------
 * TkCsvWriter
 * ----------------------------------------------------------------------- */

struct TkCsvWriter {
    GrowBuf buf;
    int     first_row; /* 1 if no rows have been written yet */

    /* Dialect settings (Story 29.4.1) */
    char    sep;       /* field separator, default ',' */
    int     use_crlf;  /* 1 = \r\n endings, 0 = \n only; default 1 (RFC 4180) */
};

TkCsvWriter *csv_writer_new(void)
{
    TkCsvWriter *w = (TkCsvWriter *)malloc(sizeof(TkCsvWriter));
    gbuf_init(&w->buf);
    w->first_row = 1;
    w->sep       = ',';
    w->use_crlf  = 1; /* RFC 4180 default */
    return w;
}

void csv_writer_set_separator(TkCsvWriter *w, char sep)
{
    w->sep = sep;
}

void csv_writer_use_crlf(TkCsvWriter *w, int enabled)
{
    w->use_crlf = enabled;
}

void csv_writer_free(TkCsvWriter *w)
{
    if (!w) return;
    gbuf_free(&w->buf);
    free(w);
}

/* Determine whether a field value needs quoting given the separator. */
static int field_needs_quoting(const char *s, char sep)
{
    for (const char *p = s; *p; p++) {
        if (*p == sep || *p == '"' || *p == '\n' || *p == '\r') return 1;
    }
    return 0;
}

void csv_writer_writerow(TkCsvWriter *w, StrArray row)
{
    if (!w->first_row) {
        if (w->use_crlf) {
            gbuf_append(&w->buf, "\r\n", 2);
        } else {
            gbuf_push(&w->buf, '\n');
        }
    }
    w->first_row = 0;

    for (uint64_t i = 0; i < row.len; i++) {
        if (i > 0) gbuf_push(&w->buf, w->sep);
        const char *f = row.data[i] ? row.data[i] : "";
        if (field_needs_quoting(f, w->sep)) {
            gbuf_push(&w->buf, '"');
            for (const char *p = f; *p; p++) {
                if (*p == '"') gbuf_push(&w->buf, '"'); /* double it */
                gbuf_push(&w->buf, *p);
            }
            gbuf_push(&w->buf, '"');
        } else {
            gbuf_append(&w->buf, f, (uint64_t)strlen(f));
        }
    }
}

const char *csv_writer_flush(TkCsvWriter *w)
{
    return gbuf_to_str(&w->buf);
}

/* -----------------------------------------------------------------------
 * csv_parse: convenience — all rows at once
 * ----------------------------------------------------------------------- */

StrArray *csv_parse(const char *data, uint64_t len, uint64_t *nrows_out)
{
    *nrows_out = 0;
    if (!data || len == 0) return NULL;

    /* Collect rows in a growable array. */
    uint64_t  rows_cap  = 16;
    uint64_t  rows_len  = 0;
    StrArray *rows      = (StrArray *)malloc(rows_cap * sizeof(StrArray));
    uint64_t  pos       = 0;

    while (pos < len) {
        RowBuf row;
        if (!parse_row(data, len, &pos, &row, ',', '"', 0)) break;
        if (rows_len == rows_cap) {
            rows_cap *= 2;
            rows = (StrArray *)realloc(rows, rows_cap * sizeof(StrArray));
        }
        rows[rows_len++] = rowbuf_to_strarray(&row);
    }

    if (rows_len == 0) {
        free(rows);
        return NULL;
    }

    *nrows_out = rows_len;
    return rows;
}
