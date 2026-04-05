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
