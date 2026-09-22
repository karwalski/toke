/*
 * xlsx_glue.c — i64-ABI wrappers for the std.xlsx module (story 135.4).
 *
 * Registered against module "xlsx" in src/stdlib_deps.c.  136.33 is why that
 * sentence is here: a glue file registered under the wrong module name leaves
 * the module unlinkable ON ITS OWN, and nothing shows it up until a program
 * imports that module and nothing else.  test/conform/C031 compiles such a
 * program as its first case.
 *
 * ABI notes (docs/runtime-abi.md):
 *   §3  a `str` is a plain NUL-terminated char*, cast through i64.
 *   §4  an array is a length-prefixed i64 block; the handle points at data[0]
 *       and the length sits at handle[-1] (tk_array.h).  A `@(byte)` stores
 *       ONE BYTE PER i64 SLOT (bytes_rt.h) — hence tk_bytes_unpack rather
 *       than a raw buffer.
 *   §5  a struct is a flat i64 block, one slot per declared field, in
 *       declaration order.
 *
 * §5 is the one that bites.  A `$xlsxcell` built here MUST have exactly the
 * six slots stdlib/xlsx.tki declares, in that order:
 *
 *     slot 0  ref      str
 *     slot 1  row      i64
 *     slot 2  col      i64
 *     slot 3  celltype str
 *     slot 4  raw      str
 *     slot 5  value    str
 *
 * If the shapes disagree the program still compiles and then reads whatever
 * sits at the offset — 127.86 read the first eight bytes of an id string as a
 * size and reported 49, 50, 51 for buffers of 128, 256 and 512 bytes.  There
 * is no diagnostic for that; the only defence is that the .tki and this file
 * change together, and that C031 reads EVERY field and checks its value.
 */

#include "xlsx.h"
#include "tk_array.h"
#include "bytes_rt.h"
#include "capabilities.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Number of i64 slots in a $xlsxcell — keep in sync with stdlib/xlsx.tki. */
#define TK_XLSXCELL_SLOTS 6

static int64_t dup_str(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    char *p = (char *)malloc(n + 1);
    if (!p) return 0;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return (int64_t)(intptr_t)p;
}

/* xlsx.open(data:@(byte)) -> $xlsxbook!$xlsxerr
 *
 * 0 on any rejection, which is what the compiled error-union ABI lowers the
 * `$err` arm to.  xlsx.lasterr() carries the reason. */
int64_t tk_xlsx_open_w(int64_t data)
{
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(data, &buf);
    if (!buf) return 0;
    TkXlsxBook *wb = xlsx_open_mem(buf, n);
    free(buf);
    return (int64_t)(intptr_t)wb;
}

/* xlsx.openfile(path:str) -> $xlsxbook!$xlsxerr
 *
 * The route a LARGE workbook takes.  A toke `@(byte)` costs eight bytes per
 * byte, so `xlsx.open` on a 40 MiB workbook wants 320 MiB of toke array
 * before a cell is read; this reads the container in C and costs the file's
 * own size once.  Gated on fs.read like every other filesystem sink
 * (124.4c). */
int64_t tk_xlsx_openfile_w(int64_t path)
{
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return 0;
    return (int64_t)(intptr_t)xlsx_open_file((const char *)(intptr_t)path);
}

/* xlsx.sheets(wb:$xlsxbook) -> @(str)
 *
 * Tab order, which is workbook.xml's order.  A workbook with no sheets
 * yields a zero-length array, never 0: a null would be indexed. */
int64_t tk_xlsx_sheets_w(int64_t handle)
{
    TkXlsxBook *wb = (TkXlsxBook *)(intptr_t)handle;
    uint32_t n = xlsx_sheet_count(wb);
    if (!wb || n == 0) return tk_arr_alloc(0, 0);

    int64_t h = tk_arr_alloc((int64_t)n, (int64_t)n);
    if (!h) return tk_arr_alloc(0, 0);
    int64_t *block = (int64_t *)(intptr_t)h;

    uint32_t built = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *name = xlsx_sheet_name(wb, i);
        if (!name) break;
        int64_t s = dup_str(name);   /* the book owns its names and is closed
                                      * independently of this array */
        if (!s) break;
        block[built++] = s;
    }
    tk_arr_setlen(h, (int64_t)built);
    return h;
}

/* One $xlsxcell as a flat six-slot block. */
static int64_t build_cell(const TkXlsxCell *c)
{
    int64_t *slot = (int64_t *)malloc(TK_XLSXCELL_SLOTS * sizeof(int64_t));
    if (!slot) return 0;
    int64_t ref   = dup_str(c->ref);
    int64_t ctype = dup_str(c->celltype);
    int64_t raw   = dup_str(c->raw);
    int64_t value = dup_str(c->value);
    if (!ref || !ctype || !raw || !value) {
        free((void *)(intptr_t)ref);   free((void *)(intptr_t)ctype);
        free((void *)(intptr_t)raw);   free((void *)(intptr_t)value);
        free(slot);
        return 0;
    }
    slot[0] = ref;       /* .ref      */
    slot[1] = c->row;    /* .row      */
    slot[2] = c->col;    /* .col      */
    slot[3] = ctype;     /* .celltype */
    slot[4] = raw;       /* .raw      */
    slot[5] = value;     /* .value    */
    return (int64_t)(intptr_t)slot;
}

/* xlsx.rows(wb:$xlsxbook; sheet:str) -> @(@($xlsxcell))!$xlsxerr
 *
 * A RECTANGLE: every row is the same width and every position holds a cell,
 * holes included, because the sheet is sparse in the file and a consumer
 * indexing `rows.get(i).get(j)` must not have to know that.  Each cell still
 * carries its own `.ref`, `.row` and `.col`, so a consumer that wants to
 * cross-check the placement can. */
int64_t tk_xlsx_rows_w(int64_t handle, int64_t sheet)
{
    TkXlsxBook *wb = (TkXlsxBook *)(intptr_t)handle;
    if (!wb || !sheet) return 0;

    TkXlsxSheet *sh = xlsx_read_sheet(wb, (const char *)(intptr_t)sheet);
    if (!sh) return 0;

    uint32_t nr = xlsx_row_count(sh), nc = xlsx_col_count(sh);
    int64_t rows = tk_arr_alloc((int64_t)nr, (int64_t)nr);
    if (!rows) { xlsx_sheet_free(sh); return 0; }
    int64_t *rblock = (int64_t *)(intptr_t)rows;

    uint32_t built_rows = 0;
    for (uint32_t r = 0; r < nr; r++) {
        int64_t row = tk_arr_alloc((int64_t)nc, (int64_t)nc);
        if (!row) break;
        int64_t *cblock = (int64_t *)(intptr_t)row;
        uint32_t built_cells = 0;
        for (uint32_t c = 0; c < nc; c++) {
            const TkXlsxCell *cell = xlsx_cell_at(sh, r, c);
            if (!cell) break;
            int64_t v = build_cell(cell);
            if (!v) break;
            cblock[built_cells++] = v;
        }
        tk_arr_setlen(row, (int64_t)built_cells);
        rblock[built_rows++] = row;
    }
    tk_arr_setlen(rows, (int64_t)built_rows);

    xlsx_sheet_free(sh);
    return rows;
}

/* xlsx.datesystem(wb:$xlsxbook) -> str — "1900" or "1904".
 *
 * Exposed because it decides how every serial in the workbook is read, and a
 * consumer that has to guess which system it got is in exactly the position
 * this module exists to remove it from. */
int64_t tk_xlsx_datesystem_w(int64_t handle)
{
    TkXlsxBook *wb = (TkXlsxBook *)(intptr_t)handle;
    return (int64_t)(intptr_t)(xlsx_date1904(wb) ? "1904" : "1900");
}

/* xlsx.datefromserial(serial:str; system:str) -> str
 *
 * The date mapping on its own, so that the 1900 leap-year rule can be tested
 * across the days where it bites without building a fixture workbook per day.
 * `system` is "1904" for the 1904 system and anything else for 1900.
 * Returns "" for text that is not a serial. */
int64_t tk_xlsx_datefromserial_w(int64_t serial, int64_t system)
{
    char out[64];
    const char *s = (const char *)(intptr_t)serial;
    const char *sys = (const char *)(intptr_t)system;
    int d1904 = sys && !strcmp(sys, "1904");
    if (!s || !xlsx_serial_to_text(s, d1904, 0, out, sizeof out))
        return dup_str("");
    return dup_str(out);
}

/* xlsx.close(wb:$xlsxbook) -> void — releases the workbook and its zip. */
int64_t tk_xlsx_close_w(int64_t handle)
{
    xlsx_close((TkXlsxBook *)(intptr_t)handle);
    return 0;
}

/* xlsx.lasterr() -> str — why the last xlsx call failed; "" if none has. */
int64_t tk_xlsx_lasterr_w(void)
{
    const char *m = xlsx_lasterr();
    return (int64_t)(intptr_t)(m ? m : "");
}
