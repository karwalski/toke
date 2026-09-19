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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 135.2 last-error state, defined at the bottom of this file with the rest of
 * the hardening.  Declared here so that the LAX reader can also say "eof"
 * rather than leaving a stale kind behind for a caller that mixes the two. */
static void csv_err_clear(void);
static void csv_err_eof(void);
static int  csv_fail(const char *kind, uint64_t line, const char *fmt, ...);

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

static void rowbuf_free_fields(RowBuf *r)
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

/* 135.2: outcomes of one strict row parse.  The lax path (strict == 0) never
 * produces anything but RP_OK / RP_EOF, which is precisely the defect: an
 * unterminated quote and a bare quote are silent there. */
typedef enum {
    RP_OK = 0,
    RP_EOF,
    RP_UNTERMINATED,  /* quoted field never closed before EOF */
    RP_AFTERQUOTE,    /* text between a closing quote and the next delimiter */
    RP_BAREQUOTE      /* quote character part-way through an unquoted field */
} RowParse;

/* parse_row_ex — the one parser both paths use.
 *
 * *line is the 1-based PHYSICAL line and is advanced past every newline the
 * record consumes, so a record containing a quoted line break leaves it
 * several lines further on.  *start_line receives the line the record began
 * on, which is the line a caller wants named in an error.
 *
 * err_line receives the line to blame when the return value is not RP_OK:
 * for an unterminated quote that is the line the quote OPENED on, not the end
 * of the file, because the opening quote is the thing that is wrong. */
static RowParse parse_row_ex(const char *data, uint64_t len, uint64_t *pos,
                             RowBuf *row, char sep, char quote_char,
                             int lazyquotes, int strict,
                             uint64_t *line, uint64_t *start_line,
                             uint64_t *err_line)
{
    uint64_t scratch_line = 1;
    if (!line) line = &scratch_line;
    if (start_line) *start_line = *line;
    if (err_line) *err_line = *line;

    if (*pos >= len) return RP_EOF;

    GrowBuf field;
    gbuf_init(&field);
    rowbuf_init(row);

    int      in_quotes   = 0;
    int      just_closed = 0;   /* a closing quote was the previous byte */
    uint64_t quote_line  = *line;

    while (*pos < len) {
        char c = data[*pos];

        if (in_quotes) {
            if (c == quote_char) {
                /* Peek at next char to distinguish doubled-quote from close */
                if (*pos + 1 < len && data[*pos + 1] == quote_char) {
                    /* Escaped quote: qq -> q */
                    gbuf_push(&field, quote_char);
                    *pos += 2;
                } else {
                    /* Closing quote */
                    in_quotes   = 0;
                    just_closed = 1;
                    (*pos)++;
                }
            } else {
                /* RFC 4180: a quoted field may span line breaks.  The line
                 * counter still advances, so the NEXT record is numbered
                 * against the file rather than against the record count. */
                if (c == '\n') (*line)++;
                gbuf_push(&field, c);
                (*pos)++;
            }
        } else {
            if (c == quote_char) {
                if (lazyquotes && field.len > 0) {
                    /* Lenient: a quote after content is a literal character. */
                    gbuf_push(&field, c);
                    (*pos)++;
                } else if (strict && (field.len > 0 || just_closed)) {
                    /* ab"cd" style.  The lax parser opens a quoted section
                     * here and swallows everything to the next quote, which
                     * is how a description field eats the rest of the file. */
                    if (err_line) *err_line = *line;
                    gbuf_free(&field);
                    rowbuf_free_fields(row);
                    return just_closed ? RP_AFTERQUOTE : RP_BAREQUOTE;
                } else {
                    in_quotes   = 1;
                    just_closed = 0;
                    quote_line  = *line;
                    (*pos)++;
                }
            } else if (c == sep) {
                rowbuf_push(row, gbuf_to_str(&field));
                field.len   = 0; /* reset without freeing buffer */
                just_closed = 0;
                (*pos)++;
            } else if (c == '\r') {
                /* CRLF or bare CR: consume and end row */
                (*pos)++;
                if (*pos < len && data[*pos] == '\n') (*pos)++;
                (*line)++;
                rowbuf_push(row, gbuf_to_str(&field));
                gbuf_free(&field);
                return RP_OK;
            } else if (c == '\n') {
                (*pos)++;
                (*line)++;
                rowbuf_push(row, gbuf_to_str(&field));
                gbuf_free(&field);
                return RP_OK;
            } else {
                if (strict && just_closed) {
                    /* "abc"def — the lax parser appends def to abc and says
                     * nothing, so the field is quietly wrong. */
                    if (err_line) *err_line = *line;
                    gbuf_free(&field);
                    rowbuf_free_fields(row);
                    return RP_AFTERQUOTE;
                }
                gbuf_push(&field, c);
                (*pos)++;
            }
        }
    }

    if (strict && in_quotes) {
        /* EOF inside a quoted field.  The lax parser flushes what it has and
         * returns a row, so a file truncated mid-quote looks complete. */
        if (err_line) *err_line = quote_line;
        gbuf_free(&field);
        rowbuf_free_fields(row);
        return RP_UNTERMINATED;
    }

    /* EOF: flush last field */
    rowbuf_push(row, gbuf_to_str(&field));
    gbuf_free(&field);
    return RP_OK;
}

static int parse_row(const char *data, uint64_t len, uint64_t *pos,
                     RowBuf *row, char sep, char quote_char, int lazyquotes)
{
    return parse_row_ex(data, len, pos, row, sep, quote_char, lazyquotes,
                        0, NULL, NULL, NULL) == RP_OK;
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

    /* 135.2 hardening.  All zero for a reader built by csv_reader_new, which
     * is what keeps the lax path bit-for-bit what it was. */
    int         strict;        /* 1 for a reader from csv_reader_open       */
    char       *owned;         /* copied/transcoded buffer this reader owns */
    uint64_t    phys_line;     /* 1-based PHYSICAL line of the next record  */
    uint64_t    row_start;     /* physical line the last record began on    */
    int         width_known;   /* 1 once the first row has fixed the width  */
    uint64_t    width;         /* field count the first row established     */
    int         ragged_policy; /* RAGGED_ERROR / RAGGED_PAD / RAGGED_REPORT */
    int         at_end;        /* 1 once the data is exhausted              */
    char      **ragged_msgs;   /* "report" policy: one line per ragged row  */
    uint64_t    ragged_len;
    uint64_t    ragged_cap;
    TkCsvDialect dialect;      /* what this reader is actually using        */
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
    r->strict       = 0;
    r->owned        = NULL;
    r->phys_line    = 1;
    r->row_start    = 0;
    r->width_known  = 0;
    r->width        = 0;
    r->ragged_policy = 0;
    r->at_end       = 0;
    r->ragged_msgs  = NULL;
    r->ragged_len   = 0;
    r->ragged_cap   = 0;
    r->dialect.delim      = ',';
    r->dialect.quote      = '"';
    r->dialect.has_header = 0;
    r->dialect.encoding   = TK_CSV_ENC_UTF8;
    r->dialect.bom        = 0;
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
    for (uint64_t i = 0; i < r->ragged_len; i++) free(r->ragged_msgs[i]);
    free(r->ragged_msgs);
    free(r->owned);
    free(r);
}

int csv_reader_has_next(TkCsvReader *r)
{
    return r->pos < r->len;
}

static StrArray csv_reader_next_strict(TkCsvReader *r);

StrArray csv_reader_next(TkCsvReader *r)
{
    StrArray empty = {NULL, 0};
    if (r->strict) return csv_reader_next_strict(r);
    csv_err_clear();
    if (r->pos >= r->len) { csv_err_eof(); return empty; }

    RowBuf row;
    if (!parse_row(r->data, r->len, &r->pos, &row,
                   r->sep, r->quote_char, r->lazyquotes)) {
        csv_err_eof();
        return empty;
    }
    r->line_number++;
    return rowbuf_to_strarray(&row);
}

StrArray csv_reader_header(TkCsvReader *r)
{
    if (r->header_read) return r->header_cache;

    /* 135.2: opts.hasheader is load-bearing, not decorative.  Asking a reader
     * opened with hasheader=false for a header would hand back the first DATA
     * row as column names, and the caller would never know -- the same class
     * of silent wrongness as an unstripped byte-order mark. */
    if (r->strict && !r->dialect.has_header) {
        StrArray empty = {NULL, 0};
        csv_fail("opts", 0,
                 "this reader was opened with opts.hasheader false, so row 0 "
                 "is data; do not call csv.header on it");
        return empty;
    }

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

/* =======================================================================
 * 135.2 — surviving real-world exports
 *
 * Everything below exists to replace SILENCE with a refusal that names the
 * line and the problem.  Six failures were hit in practice by the consumer
 * (loke, MK21) and each has a fixture in test/conform/C008_csv_hardening.sh:
 * a UTF-8 BOM, a CP1252 export, semicolon separation, a newline inside a
 * quoted field, ragged rows, and leading-zero identifiers.
 *
 * The lax path (csv_reader_new / csv.reader) is deliberately UNCHANGED.  It
 * is both back-compatibility for every existing caller and this module's own
 * negative control: C008 runs the same fixtures through it and asserts they
 * are accepted, because a check nobody has watched fail proves nothing.
 * ======================================================================= */


/* ── last-error state ───────────────────────────────────────────────────
 * The compiled T!E ABI carries no payload (127.97), so the $err arm binds
 * nothing and these three values ARE the error.  Process-wide, like
 * zip_lasterr and file_lasterr: std.csv has no thread story yet, and a
 * per-reader slot would not cover csv_sniff, which has no reader. */

static char     g_csv_err[512] = "";
static const char *g_csv_kind  = "ok";
static uint64_t g_csv_errline  = 0;

static void csv_err_clear(void)
{
    g_csv_err[0]  = '\0';
    g_csv_kind    = "ok";
    g_csv_errline = 0;
}

/* Every refusal in this file goes through here, so every one of them carries
 * a kind, a line and a message, and none can be added that does not. */
static int csv_fail(const char *kind, uint64_t line, const char *fmt, ...)
{
    va_list ap;
    char body[400];
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);

    g_csv_kind    = kind;
    g_csv_errline = line;
    if (line)
        snprintf(g_csv_err, sizeof g_csv_err, "csv: line %llu: %s",
                 (unsigned long long)line, body);
    else
        snprintf(g_csv_err, sizeof g_csv_err, "csv: %s", body);
    return 0;
}

/* End of data is not a failure, so it carries no message — but it IS a
 * distinct kind, which is the whole point: before this, a refusal and the end
 * of the file were the same empty answer. */
static void csv_err_eof(void)
{
    g_csv_err[0]  = '\0';
    g_csv_kind    = "eof";
    g_csv_errline = 0;
}

const char *csv_lasterr(void)     { return g_csv_err; }
const char *csv_lasterrkind(void) { return g_csv_kind; }
uint64_t    csv_lasterrline(void) { return g_csv_errline; }

/* ── encodings ─────────────────────────────────────────────────────────── */

enum { ENC_UTF8 = 0, ENC_CP1252, ENC_LATIN1, ENC_BINARY };

static int enc_from_token(const char *t, int *out)
{
    if (!t || !*t)                        { *out = ENC_UTF8;   return 1; }
    if (!strcmp(t, "utf-8") ||
        !strcmp(t, "utf8"))               { *out = ENC_UTF8;   return 1; }
    if (!strcmp(t, "cp1252") ||
        !strcmp(t, "windows-1252"))       { *out = ENC_CP1252; return 1; }
    if (!strcmp(t, "latin-1") ||
        !strcmp(t, "latin1")  ||
        !strcmp(t, "iso-8859-1"))         { *out = ENC_LATIN1; return 1; }
    if (!strcmp(t, "binary"))             { *out = ENC_BINARY; return 1; }
    return 0;
}

/* Strict UTF-8 scan.  Rejects overlong forms, surrogates and anything above
 * U+10FFFF as well as malformed sequences, because a decoder that accepts
 * those is a decoder that turns a CP1252 export into plausible nonsense.
 *
 * On failure sets *bad_off to the offending byte and *bad_line to its
 * 1-based line, which is what makes the message actionable. */
static int utf8_scan(const unsigned char *p, uint64_t n,
                     uint64_t *bad_off, uint64_t *bad_line)
{
    uint64_t line = 1;
    for (uint64_t i = 0; i < n; ) {
        unsigned char c = p[i];
        if (c == '\n') { line++; i++; continue; }
        if (c < 0x80)  { i++; continue; }

        uint64_t need;
        uint32_t cp;
        if      ((c & 0xE0) == 0xC0) { need = 1; cp = (uint32_t)(c & 0x1F); }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = (uint32_t)(c & 0x0F); }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = (uint32_t)(c & 0x07); }
        else { *bad_off = i; *bad_line = line; return 0; } /* 0x80..0xBF, 0xF8+ */

        if (i + need >= n) {          /* need bytes i+1..i+need to exist */
            *bad_off = i; *bad_line = line; return 0;      /* truncated */
        }
        for (uint64_t k = 1; k <= need; k++) {
            unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80) { *bad_off = i; *bad_line = line; return 0; }
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        /* Overlong, surrogate, out of range. */
        if ((need == 1 && cp < 0x80) ||
            (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) ||
            cp > 0x10FFFF) { *bad_off = i; *bad_line = line; return 0; }
        i += need + 1;
    }
    return 1;
}

/* Windows-1252, 0x80..0x9F.  0 means "undefined in CP1252" — those five byte
 * values are refused rather than substituted, since a file containing one is
 * not really CP1252 and guessing would put us back where we started. */
static const uint16_t CP1252_HIGH[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178
};

static void utf8_emit(GrowBuf *b, uint32_t cp)
{
    if (cp < 0x80) {
        gbuf_push(b, (char)cp);
    } else if (cp < 0x800) {
        gbuf_push(b, (char)(0xC0 | (cp >> 6)));
        gbuf_push(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        gbuf_push(b, (char)(0xE0 | (cp >> 12)));
        gbuf_push(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        gbuf_push(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Transcode a single-byte legacy encoding to UTF-8.  Newlines are neither
 * added nor removed, so LINE NUMBERS SURVIVE: an error reported against the
 * transcoded text names the same line as in the original file. */
static char *transcode_single_byte(const unsigned char *p, uint64_t n,
                                   int enc, uint64_t *out_len)
{
    GrowBuf b;
    gbuf_init(&b);
    uint64_t line = 1;
    for (uint64_t i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == '\n') line++;
        if (c < 0x80) { gbuf_push(&b, (char)c); continue; }
        if (enc == ENC_LATIN1) { utf8_emit(&b, c); continue; }
        /* CP1252 */
        if (c < 0xA0) {
            uint16_t cp = CP1252_HIGH[c - 0x80];
            if (cp == 0) {
                csv_fail("encoding", line,
                         "byte 0x%02X at offset %llu is undefined in CP1252; "
                         "the file is not CP1252 -- try encoding \"latin-1\"",
                         c, (unsigned long long)i);
                gbuf_free(&b);
                return NULL;
            }
            utf8_emit(&b, cp);
        } else {
            utf8_emit(&b, c);   /* 0xA0..0xFF agree with Latin-1 */
        }
    }
    *out_len = b.len;
    char *out = gbuf_to_str(&b);   /* NUL-terminated, but len is authoritative */
    gbuf_free(&b);
    return out;
}

/* ── delimiter sniffing ─────────────────────────────────────────────────
 *
 * Counts each candidate OUTSIDE quotes, per line, over the first
 * TK_CSV_SNIFF_LINES lines.  A candidate is scored by (how many of those
 * lines share its modal count, that modal count); the modal count must be
 * non-zero.  Using the mode rather than requiring every line to agree is
 * what lets a RAGGED file still be sniffed, which is the common case.
 *
 * Two candidates that score identically are AMBIGUOUS and the sniff is
 * refused, because picking one would be exactly the guess this story exists
 * to remove. */

static const char CSV_CANDIDATES[] = { ',', ';', '\t', '|' };
#define CSV_NCAND ((int)(sizeof CSV_CANDIDATES / sizeof CSV_CANDIDATES[0]))

typedef struct { int matching; int mode; } CandScore;

static void sniff_counts(const char *data, uint64_t len, char quote,
                         int counts[CSV_NCAND][TK_CSV_SNIFF_LINES],
                         int *nlines_out)
{
    int nlines = 0;
    int in_quotes = 0;
    for (int c = 0; c < CSV_NCAND; c++)
        for (int l = 0; l < TK_CSV_SNIFF_LINES; l++) counts[c][l] = 0;

    for (uint64_t i = 0; i < len && nlines < TK_CSV_SNIFF_LINES; i++) {
        char ch = data[i];
        if (in_quotes) {
            if (ch == quote) {
                if (i + 1 < len && data[i + 1] == quote) i++;
                else in_quotes = 0;
            }
            continue;
        }
        if (ch == quote) { in_quotes = 1; continue; }
        if (ch == '\n') { nlines++; continue; }
        if (ch == '\r') continue;
        for (int c = 0; c < CSV_NCAND; c++)
            if (ch == CSV_CANDIDATES[c]) counts[c][nlines]++;
    }
    /* A trailing line with no newline still counts, provided it had content. */
    if (nlines < TK_CSV_SNIFF_LINES && len > 0 &&
        data[len - 1] != '\n' && data[len - 1] != '\r') nlines++;
    *nlines_out = nlines;
}

static CandScore score_candidate(const int *counts, int nlines)
{
    CandScore best; best.matching = 0; best.mode = 0;
    for (int i = 0; i < nlines; i++) {
        if (counts[i] == 0) continue;
        int m = 0;
        for (int j = 0; j < nlines; j++) if (counts[j] == counts[i]) m++;
        if (m > best.matching || (m == best.matching && counts[i] > best.mode)) {
            best.matching = m;
            best.mode     = counts[i];
        }
    }
    return best;
}

/* ── csv_sniff ─────────────────────────────────────────────────────────── */

int csv_sniff(const char *data, uint64_t len, TkCsvDialect *out)
{
    csv_err_clear();
    if (!out) return csv_fail("empty", 0, "no dialect buffer supplied");

    out->delim      = ',';
    out->quote      = '"';
    out->has_header = 0;
    out->encoding   = TK_CSV_ENC_UTF8;
    out->bom        = 0;

    if (!data || len == 0)
        return csv_fail("empty", 0, "no data to sniff");

    const unsigned char *p = (const unsigned char *)data;

    /* A UTF-16 file is the one case where every later answer would be noise:
     * the delimiter counts, the encoding guess and the field text would all
     * be wrong together.  Name it instead. */
    if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        out->encoding = TK_CSV_ENC_UTF16LE;
        return csv_fail("encoding", 1,
                        "UTF-16LE byte-order mark: std.csv reads UTF-8, "
                        "CP1252 and Latin-1 only");
    }
    if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        out->encoding = TK_CSV_ENC_UTF16BE;
        return csv_fail("encoding", 1,
                        "UTF-16BE byte-order mark: std.csv reads UTF-8, "
                        "CP1252 and Latin-1 only");
    }

    uint64_t off = 0;
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        out->bom      = 1;
        out->encoding = TK_CSV_ENC_UTF8_BOM;
        off           = 3;
    }

    /* Encoding is REPORTED, not enforced, by the sniffer: "not-utf-8" is the
     * answer that lets a caller pick cp1252 and come back.  Enforcement is
     * csv_reader_open's job. */
    uint64_t bad_off = 0, bad_line = 0;
    if (!utf8_scan(p + off, len - off, &bad_off, &bad_line))
        out->encoding = TK_CSV_ENC_UNKNOWN;

    int counts[CSV_NCAND][TK_CSV_SNIFF_LINES];
    int nlines = 0;
    sniff_counts(data + off, len - off, '"', counts, &nlines);
    if (nlines == 0)
        return csv_fail("empty", 1, "no lines to sniff");

    CandScore sc[CSV_NCAND];
    int best = -1, second = -1;
    for (int c = 0; c < CSV_NCAND; c++) {
        sc[c] = score_candidate(counts[c], nlines);
        if (sc[c].matching == 0) continue;
        if (best < 0 || sc[c].matching > sc[best].matching ||
            (sc[c].matching == sc[best].matching && sc[c].mode > sc[best].mode)) {
            second = best;
            best   = c;
        } else if (second < 0 || sc[c].matching > sc[second].matching ||
                   (sc[c].matching == sc[second].matching &&
                    sc[c].mode > sc[second].mode)) {
            second = c;
        }
    }

    if (best < 0)
        return csv_fail("delimiter", 1,
                        "no delimiter from , ; tab or | fits the first %d "
                        "line(s); set opts.delim explicitly", nlines);

    if (second >= 0 && sc[second].matching == sc[best].matching &&
        sc[second].mode == sc[best].mode) {
        char a = CSV_CANDIDATES[best], b = CSV_CANDIDATES[second];
        return csv_fail("delimiter", 1,
                        "delimiter is ambiguous: '%s' and '%s' fit the first "
                        "%d line(s) equally well; set opts.delim explicitly",
                        a == '\t' ? "\\t" : (char[2]){a, 0},
                        b == '\t' ? "\\t" : (char[2]){b, 0}, nlines);
    }

    /* Fewer than half the examined lines agreeing is not a detection. */
    if (nlines > 1 && sc[best].matching * 2 < nlines)
        return csv_fail("delimiter", 1,
                        "no delimiter fits a majority of the first %d line(s) "
                        "(best was '%c' on %d); set opts.delim explicitly",
                        nlines, CSV_CANDIDATES[best], sc[best].matching);

    out->delim = CSV_CANDIDATES[best];

    /* has_header is A GUESS and is labelled as one everywhere it appears.
     * Rule: rows 0 and 1 have the same width, row 0 has no numeric-looking
     * field and no empty field, and row 1 has at least one numeric-looking
     * field.  csv_reader_open never consults it — the caller decides. */
    {
        uint64_t pos = off;
        RowBuf r0, r1;
        uint64_t line = 1;
        if (parse_row_ex(data, len, &pos, &r0, out->delim, out->quote, 0, 0,
                         &line, NULL, NULL) == RP_OK) {
            if (parse_row_ex(data, len, &pos, &r1, out->delim, out->quote, 0, 0,
                             &line, NULL, NULL) == RP_OK) {
                int ok = (r0.len == r1.len && r0.len > 0);
                int r1_numeric = 0;
                for (uint64_t i = 0; ok && i < r0.len; i++) {
                    const char *a = r0.fields[i];
                    if (!a || !*a) ok = 0;
                    else {
                        int all_num = 1;
                        for (const char *q = a; *q; q++)
                            if (!((*q >= '0' && *q <= '9') || *q == '.' ||
                                  *q == '-' || *q == '+')) { all_num = 0; break; }
                        if (all_num) ok = 0;   /* row 0 looks like data */
                    }
                }
                for (uint64_t i = 0; i < r1.len; i++) {
                    const char *b = r1.fields[i];
                    if (!b || !*b) continue;
                    int all_num = 1;
                    for (const char *q = b; *q; q++)
                        if (!((*q >= '0' && *q <= '9') || *q == '.' ||
                              *q == '-' || *q == '+')) { all_num = 0; break; }
                    if (all_num) { r1_numeric = 1; break; }
                }
                out->has_header = (ok && r1_numeric) ? 1 : 0;
                rowbuf_free_fields(&r1);
            } else {
                /* A single-row file: a lone row of non-numeric labels is the
                 * only thing it can be, so call it a header. */
                int ok = r0.len > 0;
                for (uint64_t i = 0; ok && i < r0.len; i++) {
                    const char *a = r0.fields[i];
                    if (!a || !*a) { ok = 0; break; }
                }
                out->has_header = ok;
            }
            rowbuf_free_fields(&r0);
        }
    }

    return 1;
}

/* ── options ───────────────────────────────────────────────────────────── */

enum { RAGGED_ERROR = 0, RAGGED_PAD, RAGGED_REPORT };

void csv_opts_defaults(TkCsvOpts *opts)
{
    if (!opts) return;
    opts->delim      = "";        /* sniff, and report what was sniffed */
    opts->quote      = "\"";
    opts->encoding   = "utf-8";   /* STRICT: mojibake is a refusal */
    opts->bom        = "strip";
    opts->ragged     = "error";
    opts->has_header = 1;
}

/* A one-character option, or a refusal naming the field and what arrived.
 * Silently taking the first byte of a longer string is how "\\t" ends up
 * meaning backslash. */
static int one_char_opt(const char *v, const char *field, char dflt, char *out)
{
    if (!v || !*v) { *out = dflt; return 1; }
    if (v[1] != '\0') {
        /* "\t" spelled out is common enough in configuration to accept. */
        if (!strcmp(v, "\\t")) { *out = '\t'; return 1; }
        if (!strcmp(v, "tab")) { *out = '\t'; return 1; }
        return csv_fail("opts", 0,
                        "opts.%s must be one character, got \"%s\"", field, v);
    }
    *out = v[0];
    return 1;
}

/* ── csv_reader_open ───────────────────────────────────────────────────── */

TkCsvReader *csv_reader_open(const char *data, uint64_t len,
                             const TkCsvOpts *opts)
{
    TkCsvOpts defaults;
    csv_err_clear();
    if (!opts) { csv_opts_defaults(&defaults); opts = &defaults; }
    if (!data) { csv_fail("empty", 0, "no data"); return NULL; }

    /* 1. Option tokens.  An unrecognised token is a refusal, never a default:
     *    a policy that silently became "error" would be indistinguishable
     *    from one the caller chose. */
    char delim = 0, quote = 0;
    if (!one_char_opt(opts->quote, "quote", '"', &quote)) return NULL;
    if (opts->delim && *opts->delim) {
        if (!one_char_opt(opts->delim, "delim", ',', &delim)) return NULL;
    }

    int enc = ENC_UTF8;
    if (!enc_from_token(opts->encoding, &enc)) {
        csv_fail("opts", 0,
                 "opts.encoding \"%s\" is not one of utf-8, cp1252, latin-1, "
                 "binary", opts->encoding);
        return NULL;
    }

    int bom_policy; /* 0 strip, 1 keep, 2 error */
    const char *bt = (opts->bom && *opts->bom) ? opts->bom : "strip";
    if      (!strcmp(bt, "strip")) bom_policy = 0;
    else if (!strcmp(bt, "keep"))  bom_policy = 1;
    else if (!strcmp(bt, "error")) bom_policy = 2;
    else { csv_fail("opts", 0,
                    "opts.bom \"%s\" is not one of strip, keep, error", bt);
           return NULL; }

    int ragged;
    const char *rt = (opts->ragged && *opts->ragged) ? opts->ragged : "error";
    if      (!strcmp(rt, "error"))  ragged = RAGGED_ERROR;
    else if (!strcmp(rt, "pad"))    ragged = RAGGED_PAD;
    else if (!strcmp(rt, "report")) ragged = RAGGED_REPORT;
    else { csv_fail("opts", 0,
                    "opts.ragged \"%s\" is not one of pad, error, report", rt);
           return NULL; }

    const unsigned char *p = (const unsigned char *)data;

    /* 2. Byte-order mark.  Handled before anything reads a field, because a
     *    BOM left in place is exactly the defect: the first header becomes a
     *    name nothing matches and the column mapping fails on a file that
     *    looks correct in every editor. */
    int has_bom = (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF);
    uint64_t off = 0;
    if (len >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) ||
                     (p[0] == 0xFE && p[1] == 0xFF))) {
        csv_fail("encoding", 1,
                 "UTF-16 byte-order mark: std.csv reads UTF-8, CP1252 and "
                 "Latin-1 only");
        return NULL;
    }
    if (has_bom) {
        if (bom_policy == 2) {
            csv_fail("bom", 1,
                     "UTF-8 byte-order mark present and opts.bom is \"error\"");
            return NULL;
        }
        if (bom_policy == 0) off = 3;
    }

    /* 3. Encoding.  THIS IS THE LOUD FAILURE the requirement asks for: under
     *    the default utf-8 the whole buffer is validated up front, so a
     *    CP1252 merchant name is REFUSED with its line and byte rather than
     *    arriving as mojibake that nothing downstream can detect. */
    char    *owned = NULL;
    uint64_t olen  = 0;
    if (enc == ENC_UTF8) {
        uint64_t bad_off = 0, bad_line = 0;
        if (!utf8_scan(p + off, len - off, &bad_off, &bad_line)) {
            csv_fail("encoding", bad_line,
                     "byte 0x%02X at offset %llu is not valid UTF-8; if this "
                     "is a legacy export set opts.encoding to \"cp1252\" or "
                     "\"latin-1\"",
                     p[off + bad_off], (unsigned long long)(off + bad_off));
            return NULL;
        }
        olen  = len - off;
        owned = (char *)malloc(olen + 1);
        if (!owned) { csv_fail("empty", 0, "out of memory"); return NULL; }
        if (olen) memcpy(owned, data + off, olen);
        owned[olen] = '\0';
    } else if (enc == ENC_BINARY) {
        olen  = len - off;
        owned = (char *)malloc(olen + 1);
        if (!owned) { csv_fail("empty", 0, "out of memory"); return NULL; }
        if (olen) memcpy(owned, data + off, olen);
        owned[olen] = '\0';
    } else {
        owned = transcode_single_byte(p + off, len - off, enc, &olen);
        if (!owned) return NULL;   /* csv_fail already set, with the line */
    }

    /* 4. Delimiter.  Sniffed only when the caller left it blank, and the
     *    result is reported back through csv_reader_dialect so that a
     *    sniffed delimiter is never invisible. */
    TkCsvDialect d;
    d.delim      = delim ? delim : ',';
    d.quote      = quote;
    d.has_header = opts->has_header ? 1 : 0;
    d.encoding   = has_bom ? TK_CSV_ENC_UTF8_BOM : TK_CSV_ENC_UTF8;
    d.bom        = has_bom;
    if (enc == ENC_CP1252)      d.encoding = "cp1252";
    else if (enc == ENC_LATIN1) d.encoding = "latin-1";
    else if (enc == ENC_BINARY) d.encoding = "binary";

    if (!delim) {
        TkCsvDialect sn;
        if (!csv_sniff(owned, olen, &sn)) { free(owned); return NULL; }
        d.delim = sn.delim;
    }
    csv_err_clear();

    TkCsvReader *r = csv_reader_new(owned, olen);
    if (!r) { free(owned); csv_fail("empty", 0, "out of memory"); return NULL; }
    r->owned         = owned;
    r->strict        = 1;
    r->sep           = d.delim;
    r->quote_char    = d.quote;
    r->lazyquotes    = 0;
    r->ragged_policy = ragged;
    r->dialect       = d;
    r->phys_line     = 1;
    return r;
}

TkCsvDialect csv_reader_dialect(const TkCsvReader *r)
{
    TkCsvDialect d;
    d.delim = ','; d.quote = '"'; d.has_header = 0;
    d.encoding = TK_CSV_ENC_UTF8; d.bom = 0;
    if (!r) return d;
    return r->dialect;
}

int csv_reader_at_end(const TkCsvReader *r)
{
    if (!r) return 1;
    /* at_end is also set by a REFUSAL, because the buffer position after one
     * is not trustworthy.  A caller's loop therefore stops on a refusal
     * instead of spinning, and csv_lasterrkind says which of the two it was. */
    return r->at_end || r->pos >= r->len;
}

uint64_t csv_reader_ragged_count(const TkCsvReader *r)
{
    return r ? r->ragged_len : 0;
}

const char *csv_reader_ragged_at(const TkCsvReader *r, uint64_t i)
{
    if (!r || i >= r->ragged_len) return "";
    return r->ragged_msgs[i];
}

static void ragged_record(TkCsvReader *r, uint64_t line, uint64_t got,
                          uint64_t want)
{
    if (r->ragged_len == r->ragged_cap) {
        uint64_t nc = r->ragged_cap ? r->ragged_cap * 2 : 8;
        char **nm = (char **)realloc(r->ragged_msgs, nc * sizeof(char *));
        if (!nm) return;
        r->ragged_msgs = nm;
        r->ragged_cap  = nc;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "line %llu: %llu fields, expected %llu",
             (unsigned long long)line, (unsigned long long)got,
             (unsigned long long)want);
    size_t n = strlen(buf);
    char *copy = (char *)malloc(n + 1);
    if (!copy) return;
    memcpy(copy, buf, n + 1);
    r->ragged_msgs[r->ragged_len++] = copy;
}

/* The strict read.  Every exit either returns a row or sets a kind: an empty
 * row with kind "eof" means the data ran out, and an empty row with any other
 * kind means a refusal.  Those two were the same answer before this story. */
static StrArray csv_reader_next_strict(TkCsvReader *r)
{
    StrArray empty = {NULL, 0};

    /* A REFUSAL ENDS THE READER, and reading past it does not erase why.
     * The buffer position after a quoting failure is not trustworthy, so
     * carrying on would invent rows; and clearing the state here would lose
     * the one thing the caller still needs. */
    if (r->at_end) return empty;

    csv_err_clear();

    if (r->pos >= r->len) {
        r->at_end = 1;
        csv_err_eof();
        return empty;
    }

    RowBuf   row;
    uint64_t start = r->phys_line, errline = r->phys_line;
    RowParse rc = parse_row_ex(r->data, r->len, &r->pos, &row,
                               r->sep, r->quote_char, 0, 1,
                               &r->phys_line, &start, &errline);
    if (rc == RP_EOF) {
        r->at_end = 1;
        csv_err_eof();
        return empty;
    }
    if (rc != RP_OK) {
        r->at_end = 1;   /* the buffer position is no longer trustworthy */
        switch (rc) {
        case RP_UNTERMINATED:
            csv_fail("quote", errline,
                     "quoted field opened here is never closed before the end "
                     "of the data");
            break;
        case RP_AFTERQUOTE:
            csv_fail("quote", errline,
                     "text after a closing quote; a quoted field must end at "
                     "the delimiter or the line break");
            break;
        default:
            csv_fail("quote", errline,
                     "quote character part-way through an unquoted field; "
                     "quote the whole field or remove it");
            break;
        }
        return empty;
    }

    r->line_number++;
    r->row_start = start;

    /* Ragged-row policy.  The width is whatever the FIRST row had, header or
     * not, so the policy applies to a headerless file too. */
    if (!r->width_known) {
        r->width_known = 1;
        r->width       = row.len;
        return rowbuf_to_strarray(&row);
    }

    if (row.len == r->width) return rowbuf_to_strarray(&row);

    if (r->ragged_policy == RAGGED_REPORT) {
        ragged_record(r, start, row.len, r->width);
        return rowbuf_to_strarray(&row);   /* as it stands: nothing invented */
    }

    if (r->ragged_policy == RAGGED_PAD && row.len < r->width) {
        while (row.len < r->width) {
            char *e = (char *)malloc(1);
            if (!e) break;
            e[0] = '\0';
            rowbuf_push(&row, e);
        }
        ragged_record(r, start, row.len, r->width);  /* padded, and recorded */
        return rowbuf_to_strarray(&row);
    }

    if (r->ragged_policy == RAGGED_PAD) {
        /* Too LONG.  Padding cannot shorten a row, and truncating would
         * discard a field silently, which is the failure mode this story
         * exists to remove. */
        csv_fail("ragged", start,
                 "%llu fields, expected %llu; opts.ragged \"pad\" can lengthen "
                 "a short row but will not discard fields from a long one",
                 (unsigned long long)row.len, (unsigned long long)r->width);
    } else {
        csv_fail("ragged", start,
                 "%llu fields, expected %llu", (unsigned long long)row.len,
                 (unsigned long long)r->width);
    }
    rowbuf_free_fields(&row);
    r->at_end = 1;
    return empty;
}
