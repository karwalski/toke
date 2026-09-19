#ifndef TK_STDLIB_CSV_H
#define TK_STDLIB_CSV_H

/*
 * csv.h — C interface for the std.csv standard library module.
 *
 * Type mappings:
 *   [Str]    = StrArray  (defined in str.h)
 *   [Byte]   = ByteArray (defined in str.h)
 *   Str      = const char *  (null-terminated UTF-8)
 *
 * Implementation is RFC 4180 compliant and self-contained (no external
 * dependencies beyond libc).
 *
 * Story: 16.1.1
 */

#include "str.h"

typedef struct TkCsvReader TkCsvReader;
typedef struct TkCsvWriter TkCsvWriter;

/* Reader */

/* csv_reader_new: create a reader that parses from the given in-memory buffer.
 * data is not copied; caller must keep it alive for the lifetime of the reader.
 * Returns a heap-allocated reader; caller must call csv_reader_free(). */
TkCsvReader *csv_reader_new(const char *data, uint64_t len);
void         csv_reader_free(TkCsvReader *r);

/* csv_reader_has_next: returns 1 if there is at least one more row to read,
 * 0 at EOF. */
int          csv_reader_has_next(TkCsvReader *r);

/* csv_reader_next: reads and returns the next row as a StrArray.
 * Each field is a heap-allocated, NUL-terminated string; caller owns them.
 * Returns an empty StrArray (len==0, data==NULL) at EOF. */
StrArray     csv_reader_next(TkCsvReader *r);

/* csv_reader_header: returns the first row.
 * If has_next has been called zero times this reads the first row and caches
 * it so that subsequent next() calls begin from the second row.
 * If the reader has already advanced past row 0, returns the cached copy. */
StrArray     csv_reader_header(TkCsvReader *r);

/* csv_reader_set_separator: change the field delimiter (default ',').
 * Must be called before reading any rows.  Pass '\t' for TSV. */
void         csv_reader_set_separator(TkCsvReader *r, char sep);

/* csv_reader_set_quote: change the quote character (default '"').
 * Must be called before reading any rows. */
void         csv_reader_set_quote(TkCsvReader *r, char ch);

/* csv_reader_lazyquotes: if enabled (non-zero), a bare quote character that
 * appears inside an unquoted field is treated as a literal character rather
 * than a parse error.  Disabled by default. */
void         csv_reader_lazyquotes(TkCsvReader *r, int enabled);

/* csv_reader_line_number: return the current 1-based line number.
 * Returns 0 before any rows have been read; increments by 1 after each
 * successful csv_reader_next() call. */
uint64_t     csv_reader_line_number(TkCsvReader *r);

/* Writer */

/* csv_writer_new: create an empty writer.
 * Caller must call csv_writer_free() when done. */
TkCsvWriter *csv_writer_new(void);
void         csv_writer_free(TkCsvWriter *w);

/* csv_writer_writerow: append one row to the writer's internal buffer.
 * Fields containing ',', '"', '\n', or '\r' are quoted; '"' inside a quoted
 * field is doubled (""). */
void         csv_writer_writerow(TkCsvWriter *w, StrArray row);

/* csv_writer_flush: return the accumulated CSV as a heap-allocated
 * NUL-terminated string. Caller owns the returned pointer.
 * The writer's buffer is NOT cleared; successive flush() calls return the
 * full accumulated text. */
const char  *csv_writer_flush(TkCsvWriter *w);

/* csv_writer_set_separator: change the field delimiter (default ',').
 * Must be called before writing any rows. */
void         csv_writer_set_separator(TkCsvWriter *w, char sep);

/* csv_writer_use_crlf: if enabled (non-zero), rows are terminated with
 * "\r\n" instead of "\r\n" (the default already uses "\r\n" per RFC 4180,
 * but this function makes the choice explicit).  Pass 0 to use "\n" only. */
void         csv_writer_use_crlf(TkCsvWriter *w, int enabled);

/* Convenience */

/* csv_parse: parse an entire CSV buffer and return all rows as a
 * heap-allocated array of StrArray.  *nrows_out is set to the number of rows.
 * Each field string and the array itself are heap-allocated; caller owns them.
 * Returns NULL (and sets *nrows_out=0) on empty input. */
StrArray    *csv_parse(const char *data, uint64_t len, uint64_t *nrows_out);

/* -----------------------------------------------------------------------
 * 135.2 — hardening for real-world exports
 *
 * `csv_reader_new` is the LAX path and keeps its behaviour exactly: it
 * validates nothing, so a CP1252 byte, a BOM, an unterminated quote and a
 * short row all come back as content.  That is what every existing caller
 * already depends on, and it is also this module's own negative control —
 * test/conform/C008 runs the awkward fixtures through it and asserts they are
 * accepted, because a check that cannot be seen failing proves nothing.
 *
 * `csv_reader_open` is the STRICT path.  Everything below exists so that a
 * bank or government export is either parsed correctly or REFUSED with a
 * message naming the line and the problem.  Silence is the failure mode.
 * ----------------------------------------------------------------------- */

/* What csv_sniff found.  Mirrors the $csvdialect struct in stdlib/csv.tki,
 * field for field and in order — csv_glue.c builds that block from this. */
typedef struct {
    char        delim;      /* the delimiter that fits every examined line   */
    char        quote;      /* quote character (only '"' is ever detected)   */
    int         has_header; /* 1 if row 0 looks like a header — A GUESS      */
    const char *encoding;   /* static token, see TK_CSV_ENC_* below          */
    int         bom;        /* 1 if the buffer starts with a UTF-8 BOM       */
} TkCsvDialect;

/* Encoding tokens.  Static strings, so a caller may compare pointers or text. */
#define TK_CSV_ENC_UTF8     "utf-8"
#define TK_CSV_ENC_UTF8_BOM "utf-8-bom"
#define TK_CSV_ENC_UNKNOWN  "not-utf-8"
#define TK_CSV_ENC_UTF16LE  "utf-16le"
#define TK_CSV_ENC_UTF16BE  "utf-16be"

/* Options for csv_reader_open.  Mirrors the $csvopts struct in
 * stdlib/csv.tki, field for field and in order.
 *
 * Every field is a token rather than an enum so that an unrecognised value is
 * a REJECTION with the offending text in the message, not a silent fallback
 * to a default.  NULL or "" selects the documented default.
 *
 *   delim    one character, or "" to sniff it (the result is reported back by
 *            csv_reader_dialect, so a sniffed delimiter is never invisible)
 *   quote    one character; default '"'
 *   encoding "utf-8" (default, STRICT — invalid sequences are refused),
 *            "cp1252", "latin-1" / "iso-8859-1" (both transcoded to UTF-8),
 *            or "binary" (no validation, bytes pass through unchanged)
 *   bom      "strip" (default) | "keep" | "error"
 *   ragged   "error" (default) | "pad" | "report"
 *   has_header  whether row 0 is a header.  NEVER guessed by the reader:
 *            csv_sniff reports a guess, the caller decides.
 */
typedef struct {
    const char *delim;
    const char *quote;
    const char *encoding;
    const char *bom;
    const char *ragged;
    int         has_header;
} TkCsvOpts;

/* Fill opts with the documented defaults. */
void csv_opts_defaults(TkCsvOpts *opts);

/* csv_sniff: examine the first TK_CSV_SNIFF_LINES lines and report the
 * dialect.  Returns 1 and fills *out on success; returns 0 and sets the
 * last-error state (csv_lasterr / csv_lasterrkind / csv_lasterrline) when the
 * delimiter cannot be determined, when two delimiters fit equally well, when
 * the buffer is empty, or when it carries a UTF-16 byte-order mark.
 *
 * Non-UTF-8 bytes are NOT a sniff failure: they are reported as encoding
 * "not-utf-8" so that a caller can choose an encoding and try again. */
#define TK_CSV_SNIFF_LINES 10
int csv_sniff(const char *data, uint64_t len, TkCsvDialect *out);

/* csv_reader_open: the strict reader.  Copies and, where the declared
 * encoding requires it, transcodes the buffer, so the caller need not keep
 * `data` alive.  Returns NULL and sets the last-error state on any refusal.
 * Free with csv_reader_free() as usual. */
TkCsvReader *csv_reader_open(const char *data, uint64_t len,
                             const TkCsvOpts *opts);

/* The dialect this reader is actually using, including a sniffed delimiter. */
TkCsvDialect csv_reader_dialect(const TkCsvReader *r);

/* 1 once every record has been consumed.  Distinguishes end-of-data from a
 * refusal: both make csv_reader_next return an empty row. */
int csv_reader_at_end(const TkCsvReader *r);

/* Ragged-row reports accumulated under the "report" policy: one
 * heap-allocated line per ragged row, e.g. "line 4: 2 fields, expected 3".
 * The reader owns them; they are freed by csv_reader_free. */
uint64_t     csv_reader_ragged_count(const TkCsvReader *r);
const char  *csv_reader_ragged_at(const TkCsvReader *r, uint64_t i);

/* Last-error state.  The compiled T!E ABI carries no payload (127.97), so the
 * $err arm binds nothing readable and these accessors ARE the error value.
 *
 * csv_lasterrkind returns one stable token:
 *   "ok"        nothing has failed
 *   "eof"       csv_reader_next returned empty because the data ran out
 *   "opts"      an option token was not recognised
 *   "empty"     there was nothing to sniff
 *   "bom"       a byte-order mark, and the policy or encoding refuses it
 *   "encoding"  the bytes are not valid in the declared encoding
 *   "delimiter" no delimiter fits, or two fit equally well
 *   "quote"     an unterminated quoted field, or text after a closing quote
 *   "ragged"    a row's field count disagrees with the header's
 *
 * csv_lasterr returns the full message, which NAMES THE LINE.  Its exact
 * wording is not interface; the kind and the line number are.
 * csv_lasterrline returns the 1-based physical line, or 0 if not applicable. */
const char *csv_lasterr(void);
const char *csv_lasterrkind(void);
uint64_t    csv_lasterrline(void);

/* -----------------------------------------------------------------------
 * .tki-aligned aliases (Story 35.1.11)
 *
 * The std.csv .tki contract uses csv.reader, csv.next, etc.  The compiler
 * maps module.func → module_func, so the expected C symbols are csv_reader,
 * csv_next, etc.  The macros below provide those names as aliases for the
 * original longer forms so both spellings work.
 * ----------------------------------------------------------------------- */
#define csv_reader    csv_reader_new
#define csv_next      csv_reader_next
#define csv_header    csv_reader_header
#define csv_writer    csv_writer_new
#define csv_writerow  csv_writer_writerow
#define csv_flush     csv_writer_flush

#endif /* TK_STDLIB_CSV_H */
