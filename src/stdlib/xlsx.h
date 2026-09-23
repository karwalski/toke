/*
 * xlsx.h — C interface for the std.xlsx standard library module.
 *
 * Reads cell VALUES out of an XLSX workbook.  Nothing is vendored for this:
 * an XLSX is a zip of XML, std.zip (135.1) already opens the zip, and the
 * rest is a focused scanner over four parts —
 *
 *     xl/workbook.xml             sheet names, and the date system
 *     xl/_rels/workbook.xml.rels  r:id -> the part each sheet lives in
 *     xl/sharedStrings.xml        the string pool cells index into
 *     xl/styles.xml               the number format each cell is shown with
 *     xl/worksheets/sheetN.xml    the cells
 *
 * ADR-0015: the toolchain is C99 only, so a second vendored library was never
 * an option here and is not wanted anyway — the format's difficulty is not in
 * the XML, it is in three details that a general-purpose XML library does
 * nothing to help with.  Each one is a silent wrong answer rather than a
 * failure, so each is called out where it is handled:
 *
 *   1. SHARED STRINGS.  A text cell usually holds an INDEX (`t="s"`) into
 *      xl/sharedStrings.xml, not the text.  A reader that parses only the
 *      worksheet gets `4` where the column says `Opening balance`, and the
 *      numbers it gets are plausible, so nothing announces the mistake.
 *      Handled in resolve_cell() in xlsx.c.
 *
 *   2. DATES ARE SERIAL NUMBERS, AND THE 1900 SYSTEM'S LEAP-YEAR BUG IS
 *      REPRODUCED ON PURPOSE.  Lotus 1-2-3 treated 1900 as a leap year,
 *      Excel copied the bug for compatibility, and every XLSX written since
 *      encodes dates through it.  Serial 60 IS 1900-02-29, a day that never
 *      existed.  A reader that "fixes" this shifts every date at or before
 *      1900-02-28 by one day relative to Excel — and the shift is silent,
 *      because 1899-12-31 is as plausible a date as 1900-01-01.  We are
 *      decoding someone else's encoding, so we reproduce it.  See
 *      xlsx_serial_to_text() in xlsx.c, and section 4 of that file.
 *
 *   3. ROWS AND COLUMNS ARE SPARSE.  A cell carries its own reference
 *      (`r="C7"`) and an empty cell is simply ABSENT from the file.  Counting
 *      cells left to right puts D7's value in C7's column for every row that
 *      has a gap.  This module never infers position from order: every cell
 *      is placed by its parsed reference, and xlsx_read_sheet() returns a
 *      RECTANGLE with the holes filled in as TK_XLSX_EMPTY cells, so that a
 *      consumer indexing by position is right by construction rather than by
 *      remembering to check.  See xlsx_read_sheet() in xlsx.c.
 *
 * SCOPE.  Cell values only.  No formula evaluation (the cached `<v>` of a
 * formula cell is returned, which is what a data-import consumer wants), no
 * styling beyond the number format needed to tell a date from a number, no
 * charts, no pivot tables.
 *
 * SECURITY.  The archive itself is validated by std.zip, which is where the
 * traversal, entry-count, size and ratio caps live.  The caps added here are
 * the ones std.zip cannot know about: a cell count, a shared-string count,
 * and a column index, all bounding what one honest-looking part can make this
 * module allocate.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  The caller owns every returned handle.
 *
 * Story: 135.4
 */

#ifndef TK_STDLIB_XLSX_H
#define TK_STDLIB_XLSX_H

#include <stddef.h>
#include <stdint.h>

/* ── Limits ──────────────────────────────────────────────────────────────
 *
 * Compile-time on purpose, for the reason zip.h gives: a bound settable from
 * toke is under the control of the code path handling the hostile input.
 */

/* Cells materialised for ONE sheet, holes included.  xlsx_read_sheet builds a
 * rectangle, so a single cell at XFD1048576 would otherwise ask for 17 billion
 * of them.  1,000,000 covers a 50,000-row statement 20 columns wide. */
#define TK_XLSX_MAX_CELLS     1000000u

/* Entries in the shared-string pool. */
#define TK_XLSX_MAX_SHARED    1000000u

/* Highest column reference accepted.  16384 == XFD, the format's own limit. */
#define TK_XLSX_MAX_COL       16384u

/* Highest row reference accepted.  1048576 is the format's own limit. */
#define TK_XLSX_MAX_ROW       1048576u

/* Entries in xl/styles.xml's cellXfs list. */
#define TK_XLSX_MAX_XF        65536u

/* Sheets in one workbook. */
#define TK_XLSX_MAX_SHEETS    4096u

/* ── Cell kinds ──────────────────────────────────────────────────────────
 *
 * These are the strings the toke surface reports in `$xlsxcell.celltype`.
 * They
 * are a CLOSED set and they are stable: a consumer switching on them is the
 * intended use, so adding to this list is a breaking change.
 *
 * WHY `celltype` AND NOT `kind`.  A `.tki` type field named `kind` silently
 * TRUNCATES its own type's field list: the export-record scanners in
 * src/llvm.c delimit one record from the next with
 * `strstr(p + 6, "\"kind\"")`, which matches the VALUE `"kind"` of a
 * `"name"` key just as happily as the `"kind"` key itself, so every field
 * declared after it is lost.  Measured here: with `kind` fourth of six,
 * `tkc --check` accepted `.raw` and `.value` and `tkc --out` rejected both
 * as fields that do not exist.  That is a compiler defect and it is filed as
 * one; `celltype` is also the word the consumer's own request uses ("raw
 * string, type, and resolved value"), so this is not merely dodging it.
 */
#define TK_XLSX_EMPTY   "empty"   /* absent from the file; a hole we filled in */
#define TK_XLSX_STR     "str"     /* text, from the shared pool or inline     */
#define TK_XLSX_NUM     "num"     /* a number, shown as a number              */
#define TK_XLSX_DATE    "date"    /* a number, shown through a date format    */
#define TK_XLSX_BOOL    "bool"    /* t="b"; value is "true" or "false"        */
#define TK_XLSX_ERR     "err"     /* t="e"; value is Excel's own #REF! etc.   */

/* ── Types ───────────────────────────────────────────────────────────── */

/*
 * One cell.  `ref`, `row` and `col` are always populated, for every cell
 * including a filled-in hole — they are the answer to trap 3 and a consumer
 * that cross-checks them cannot be silently misaligned.
 *
 * `raw` is the literal text this module read out of the file before resolving
 * anything: the shared-string INDEX for a `t="s"` cell, the serial NUMBER for
 * a date.  It is kept because a resolution this module gets wrong is
 * undiagnosable without it, and because a consumer that wants the serial
 * rather than the rendered date should not have to re-parse the file to get
 * it.
 *
 * `value` is the resolved value rendered as text: pool text for a string, an
 * ISO-8601 date for a date, "true"/"false" for a boolean, and the number
 * verbatim for a number.  Text is the only representation a single struct
 * field can carry for all six kinds, and the numeric cases round-trip exactly
 * because they are passed through untouched rather than reformatted.
 */
typedef struct {
    char    *ref;    /* "C7"; never NULL                                   */
    int64_t  row;    /* 1-based                                            */
    int64_t  col;    /* 1-based; 3 for "C7"                                */
    char    *celltype; /* one of the TK_XLSX_* strings above; never NULL   */
    char    *raw;    /* pre-resolution text; "" for an empty cell          */
    char    *value;  /* resolved text; "" for an empty cell                */
} TkXlsxCell;

/* One sheet, materialised as a rectangle: nrows x ncols, holes filled. */
typedef struct TkXlsxSheet TkXlsxSheet;

/* Opaque workbook handle.  Definition lives in xlsx.c. */
typedef struct TkXlsxBook TkXlsxBook;

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * xlsx_open_mem — open a workbook held in memory.
 *
 * Opens the zip through std.zip (so every archive rule there applies), then
 * reads workbook.xml, its rels, sharedStrings.xml and styles.xml eagerly.
 * Worksheets are NOT read here: a workbook with fifty sheets should cost one
 * sheet's memory when one sheet is wanted.
 *
 * Returns NULL on any failure; xlsx_lasterr() says which.
 */
TkXlsxBook *xlsx_open_mem(const uint8_t *data, uint64_t len);

/*
 * xlsx_open_file — read `path` and hand it to xlsx_open_mem.
 *
 * This is the path a LARGE workbook must take.  Not because of the zip — see
 * the note in docs/stdlib/xlsx.md — but because a toke `@(byte)` stores one
 * byte per i64 slot, so routing a 40 MiB workbook through `xlsx.open` costs
 * 320 MiB of toke array before the parser has read a single cell.  Reading
 * the container in C costs the file's own size, once.
 */
TkXlsxBook *xlsx_open_file(const char *path);

/* Release a workbook and everything it owns.  NULL is a no-op. */
void xlsx_close(TkXlsxBook *wb);

/* Sheet count, and the display name of sheet `i` (NULL if out of range).
 * The order is workbook.xml's order, which is the order of the tabs. */
uint32_t    xlsx_sheet_count(const TkXlsxBook *wb);
const char *xlsx_sheet_name(const TkXlsxBook *wb, uint32_t i);

/* 1 when the workbook uses the 1904 date system, 0 for 1900. */
int xlsx_date1904(const TkXlsxBook *wb);

/*
 * xlsx_read_sheet — parse one sheet by display name into a rectangle.
 *
 * Returns NULL on failure (unknown sheet, malformed part, a cap exceeded).
 * The caller frees with xlsx_sheet_free.
 *
 * An empty sheet yields a sheet with 0 rows, never NULL, so "no rows" and
 * "failed" are never the same answer.
 */
TkXlsxSheet *xlsx_read_sheet(TkXlsxBook *wb, const char *sheet);
void         xlsx_sheet_free(TkXlsxSheet *sh);

uint32_t          xlsx_row_count(const TkXlsxSheet *sh);
uint32_t          xlsx_col_count(const TkXlsxSheet *sh);
/* Cell at 0-based (r, c) within the rectangle; NULL if out of range. */
const TkXlsxCell *xlsx_cell_at(const TkXlsxSheet *sh, uint32_t r, uint32_t c);

/*
 * xlsx_serial_to_text — the date mapping, exposed.
 *
 * `serial` is the decimal text of an Excel date serial exactly as it appears
 * in a `<v>`; `date1904` selects the workbook's date system; `timeonly`
 * renders the fractional part alone.  Writes ISO-8601 into `out` and returns
 * 1, or returns 0 and leaves `out` empty if the text is not a number.
 *
 * Exposed rather than kept private because the leap-year rule is the part of
 * this module most likely to be quietly wrong, and a test that can only reach
 * it through a fixture workbook can only afford to check a handful of dates.
 * Reached from toke as `xlsx.datefromserial`.
 */
int xlsx_serial_to_text(const char *serial, int date1904, int timeonly,
                        char *out, size_t outsz);

/*
 * xlsx_lasterr — why the most recent xlsx_* call in this process failed.
 *
 * Same reason std.zip has one: the compiled error-union ABI carries no
 * payload, so the `$err` arm binds nothing a consumer can read, and
 * "the workbook was rejected" would otherwise be indistinguishable from
 * "sheet 'Ledger' is not in this workbook".  Returns "" when nothing has
 * failed.
 */
const char *xlsx_lasterr(void);

#endif /* TK_STDLIB_XLSX_H */
