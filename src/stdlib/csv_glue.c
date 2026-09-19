/*
 * csv_glue.c — i64-ABI wrappers for std.csv module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.csv.
 */

#include "csv.h"
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "bytes_rt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * tk_csv_parse_w — parse a CSV string and return a pointer to an array of
 * StrArrays (one per row).  The toke runtime receives this as an opaque
 * pointer that it can iterate.  Returns 0 on empty/NULL input.
 *
 * Layout returned: heap block where [0] = nrows (as int64_t), followed by
 * nrows StrArray structs.  The caller (toke runtime) knows this layout.
 */
int64_t tk_csv_parse_w(int64_t data) {
    if (!data) return 0;
    /* The argument is a toke [byte] (packed), not a C string — unpack it. */
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    if (!bytes || len == 0) return 0;
    char *s = (char *)malloc(len + 1);
    if (!s) return 0;
    memcpy(s, bytes, len); s[len] = '\0';
    uint64_t nrows = 0;
    StrArray *rows = csv_parse(s, len, &nrows);
    free(s);
    if (!rows || nrows == 0) { if (rows) free(rows); return 0; }
    /* Return a toke-native [csvrow]: outer[-1]=nrows, outer[i]=*csvrow, where
     * a csvrow is a 1-field struct {fields:[str]} and [str] is the toke array
     * convention (block[-1]=len, block[i]=char*). */
    int64_t outer_h = tk_arr_alloc((int64_t)nrows, (int64_t)nrows);
    if (!outer_h) { free(rows); return 0; }
    int64_t *outer = (int64_t *)(intptr_t)outer_h;
    for (uint64_t i = 0; i < nrows; i++) {
        uint64_t fl = rows[i].len;
        int64_t fields_h = tk_arr_alloc((int64_t)fl, (int64_t)fl);
        if (!fields_h) { outer[i] = 0; continue; }
        int64_t *fields = (int64_t *)(intptr_t)fields_h;
        for (uint64_t j = 0; j < fl; j++)
            fields[j] = (int64_t)(intptr_t)rows[i].data[j];
        int64_t *cr = (int64_t *)malloc(sizeof(int64_t));
        if (!cr) { outer[i] = 0; continue; }
        cr[0] = fields_h;
        outer[i] = (int64_t)(intptr_t)cr;
    }
    free(rows);
    return outer_h;
}

/*
 * tk_csv_serialize_w — serialize a toke array-of-arrays into a CSV string.
 * Expects data to point to a packed block: [nrows i64][StrArray * nrows].
 * Returns a heap-allocated CSV string pointer, or 0 on NULL input.
 */
int64_t tk_csv_serialize_w(int64_t data) {
    if (!data) return 0;
    int64_t *block = (int64_t *)(intptr_t)data;
    int64_t nrows = block[0];
    if (nrows <= 0) return 0;
    StrArray *rows = (StrArray *)(block + 1);
    TkCsvWriter *w = csv_writer_new();
    if (!w) return 0;
    for (int64_t i = 0; i < nrows; i++) {
        csv_writer_writerow(w, rows[i]);
    }
    const char *result = csv_writer_flush(w);
    csv_writer_free(w);
    return (int64_t)(intptr_t)result;
}

/* ── 114.43: streaming reader/writer wrappers ────────────────────────────
 * The streaming API was declared in csv.tki but had no `_w` wrappers, so any
 * program using it failed to link. csv.c already implements the primitives. */

/* Build a toke [str] (block[-1]=len, block[i]=char*) from a StrArray. */
static int64_t strarray_to_tokearr(StrArray a) {
    int64_t h = tk_arr_alloc((int64_t)a.len, (int64_t)a.len);
    if (!h) return 0;
    int64_t *blk = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < a.len; i++)
        blk[i] = (int64_t)(intptr_t)a.data[i];
    return h;
}

/* csv.reader([byte] data; u8 sep) -> csvreader. csv_reader_new references the
 * data buffer, so it is kept alive for the reader's lifetime. */
int64_t tk_csv_reader_w(int64_t data, int64_t sep) {
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    char *s = (char *)malloc(len + 1);
    if (!s) return 0;
    if (bytes && len) memcpy(s, bytes, len);
    s[len] = '\0';
    TkCsvReader *r = csv_reader_new(s, len);
    if (!r) { free(s); return 0; }
    if (sep) csv_reader_set_separator(r, (char)sep);
    return (int64_t)(intptr_t)r;
}

/* csv.next(csvreader) -> csvrow!csverr — 0 (err sentinel) at end of data. */
int64_t tk_csv_next_w(int64_t reader) {
    if (!reader) return 0;
    TkCsvReader *r = (TkCsvReader *)(intptr_t)reader;
    /* 135.2: no has_next() short-circuit.  Going through csv_reader_next is
     * what sets the last-error kind, so "eof" and a refusal stop being the
     * same silent 0 to the caller. */
    StrArray row = csv_reader_next(r);
    if (!row.data || row.len == 0) return 0;
    int64_t fields = strarray_to_tokearr(row);
    int64_t *cr = (int64_t *)malloc(sizeof(int64_t));   /* csvrow = {fields:[str]} */
    if (!cr) return 0;
    cr[0] = fields;
    return (int64_t)(intptr_t)cr;
}

/* csv.header(csvreader) -> [str]!csverr */
int64_t tk_csv_header_w(int64_t reader) {
    if (!reader) return 0;
    StrArray h = csv_reader_header((TkCsvReader *)(intptr_t)reader);
    if (!h.data) return 0;
    return strarray_to_tokearr(h);
}

/* csv.writer(u8 sep) -> csvwriter */
int64_t tk_csv_writer_w(int64_t sep) {
    TkCsvWriter *w = csv_writer_new();
    if (!w) return 0;
    if (sep) csv_writer_set_separator(w, (char)sep);
    return (int64_t)(intptr_t)w;
}

/* csv.writerow(csvwriter; [str] row) -> void */
int64_t tk_csv_writerow_w(int64_t writer, int64_t row) {
    if (!writer || !row) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)row;
    int64_t n = ptr[-1];
    StrArray a;
    a.len = (uint64_t)(n > 0 ? n : 0);
    a.data = (const char **)ptr;
    csv_writer_writerow((TkCsvWriter *)(intptr_t)writer, a);
    return 0;
}

/* csv.flush(csvwriter) -> [byte] */
int64_t tk_csv_flush_w(int64_t writer) {
    if (!writer) return tk_bytes_pack((const uint8_t *)"", 0);
    const char *res = csv_writer_flush((TkCsvWriter *)(intptr_t)writer);
    if (!res) return tk_bytes_pack((const uint8_t *)"", 0);
    return tk_bytes_pack((const uint8_t *)res, strlen(res));
}

/* ── 135.2: hardened reader wrappers ─────────────────────────────────────
 *
 * ABI notes (docs/runtime-abi.md, and the same trap zip_glue.c records):
 *   §3  a `str` is a NUL-terminated char*, cast through i64.
 *   §4  an array handle points at data[0]; the length sits at handle[-1].
 *       A `@(byte)` stores ONE BYTE PER i64 SLOT, hence tk_bytes_unpack.
 *   §5  a struct is a flat i64 block, one slot per declared field, IN
 *       DECLARATION ORDER.
 *
 * §5 is the one that bites.  A slot order that disagrees with stdlib/csv.tki
 * still compiles and then reads whatever sits at the offset — 127.86 read the
 * first eight bytes of a string as an integer and reported 49.  The only
 * defence is that this file and csv.tki are edited together and that
 * test/conform/C008 reads EVERY declared field and checks the value.
 *
 *   $csvdialect            $csvopts
 *   0  delim      str      0  delim      str
 *   1  quote      str      1  quote      str
 *   2  hasheader  bool     2  encoding   str
 *   3  encoding   str      3  bom        str
 *   4  bom        bool     4  ragged     str
 *                          5  hasheader  bool
 */

#define TK_CSVDIALECT_SLOTS 5
#define TK_CSVOPTS_SLOTS    6

/* A one-character NUL-terminated toke str for a delimiter or quote, so that a
 * caller can PRINT what was detected.  A u8 would have to be turned back into
 * a character by every caller that wants to show it. */
static int64_t char_to_str(char c) {
    char *s = (char *)malloc(2);
    if (!s) return (int64_t)(intptr_t)"";
    s[0] = c; s[1] = '\0';
    return (int64_t)(intptr_t)s;
}

static int64_t dialect_to_block(const TkCsvDialect *d) {
    int64_t *b = (int64_t *)malloc(TK_CSVDIALECT_SLOTS * sizeof(int64_t));
    if (!b) return 0;
    b[0] = char_to_str(d->delim);
    b[1] = char_to_str(d->quote);
    b[2] = d->has_header ? 1 : 0;
    b[3] = (int64_t)(intptr_t)(d->encoding ? d->encoding : "");
    b[4] = d->bom ? 1 : 0;
    return (int64_t)(intptr_t)b;
}

/* csv.sniff(data:@(byte)) -> $csvdialect!$csverr
 *
 * 0 on refusal, which is what the compiled error-union ABI lowers `$err` to;
 * csv.lasterrkind / csv.lasterr / csv.lasterrline carry the reason (127.97). */
int64_t tk_csv_sniff_w(int64_t data) {
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    TkCsvDialect d;
    int ok = csv_sniff((const char *)bytes, bytes ? len : 0, &d);
    free(bytes);
    if (!ok) return 0;
    return dialect_to_block(&d);
}

/* csv.opts(delim; quote; encoding; bom; ragged; hasheader) -> $csvopts
 *
 * Six explicit arguments rather than a defaults object the caller then
 * mutates: every policy this story defines is stated at the call site, so a
 * reader of the code can see which ragged-row rule is in force without
 * looking anywhere else.  "" selects the documented default for a field. */
int64_t tk_csv_opts_w(int64_t delim, int64_t quote, int64_t encoding, int64_t bom, int64_t ragged, int64_t hasheader) {
    int64_t *b = (int64_t *)malloc(TK_CSVOPTS_SLOTS * sizeof(int64_t));
    if (!b) return 0;
    b[0] = delim    ? delim    : (int64_t)(intptr_t)"";
    b[1] = quote    ? quote    : (int64_t)(intptr_t)"";
    b[2] = encoding ? encoding : (int64_t)(intptr_t)"";
    b[3] = bom      ? bom      : (int64_t)(intptr_t)"";
    b[4] = ragged   ? ragged   : (int64_t)(intptr_t)"";
    b[5] = hasheader ? 1 : 0;
    return (int64_t)(intptr_t)b;
}

/* csv.readeropts(data:@(byte); opts:$csvopts) -> $csvreader!$csverr */
int64_t tk_csv_readeropts_w(int64_t data, int64_t opts) {
    uint8_t *bytes = NULL;
    uint64_t len = tk_bytes_unpack(data, &bytes);
    TkCsvOpts o;
    csv_opts_defaults(&o);
    if (opts) {
        const int64_t *p = (const int64_t *)(intptr_t)opts;
        o.delim      = (const char *)(intptr_t)p[0];
        o.quote      = (const char *)(intptr_t)p[1];
        o.encoding   = (const char *)(intptr_t)p[2];
        o.bom        = (const char *)(intptr_t)p[3];
        o.ragged     = (const char *)(intptr_t)p[4];
        o.has_header = p[5] ? 1 : 0;
    }
    /* csv_reader_open copies (and where needed transcodes) the buffer and
     * owns the copy, so the staging buffer is freed here rather than leaked
     * for the reader's lifetime as tk_csv_reader_w must. */
    TkCsvReader *r = csv_reader_open((const char *)bytes, bytes ? len : 0, &o);
    free(bytes);
    return (int64_t)(intptr_t)r;
}

/* csv.dialect(r:$csvreader) -> $csvdialect — including a SNIFFED delimiter,
 * so that a delimiter the caller did not state is still visible to them. */
int64_t tk_csv_dialect_w(int64_t reader) {
    TkCsvDialect d = csv_reader_dialect((const TkCsvReader *)(intptr_t)reader);
    return dialect_to_block(&d);
}

/* csv.atend(r:$csvreader) -> bool — end of data, as distinct from a refusal.
 * Both make csv.next return the `$err` arm; only this tells them apart before
 * the call, and csv.lasterrkind tells them apart after it. */
int64_t tk_csv_atend_w(int64_t reader) {
    return csv_reader_at_end((const TkCsvReader *)(intptr_t)reader) ? 1 : 0;
}

/* csv.raggedreport(r:$csvreader) -> @(str) — one line per ragged row seen so
 * far under the "report" (or "pad") policy.  A report a caller cannot read is
 * not a report, which is why this exists rather than a bare counter. */
int64_t tk_csv_raggedreport_w(int64_t reader) {
    const TkCsvReader *r = (const TkCsvReader *)(intptr_t)reader;
    uint64_t n = csv_reader_ragged_count(r);
    int64_t h = tk_arr_alloc((int64_t)n, (int64_t)n);
    if (!h) return tk_arr_alloc(0, 0);
    int64_t *blk = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < n; i++) {
        const char *m = csv_reader_ragged_at(r, i);
        size_t l = strlen(m);
        char *copy = (char *)malloc(l + 1);
        if (!copy) { blk[i] = (int64_t)(intptr_t)""; continue; }
        memcpy(copy, m, l + 1);
        blk[i] = (int64_t)(intptr_t)copy;
    }
    return h;
}

/* csv.close(r:$csvreader) -> void */
int64_t tk_csv_close_w(int64_t reader) {
    csv_reader_free((TkCsvReader *)(intptr_t)reader);
    return 0;
}

/* csv.lasterr() -> str — the message, which NAMES THE LINE. */
int64_t tk_csv_lasterr_w(void) {
    const char *m = csv_lasterr();
    return (int64_t)(intptr_t)(m ? m : "");
}

/* csv.lasterrkind() -> str — the stable token; this is the interface, the
 * wording of lasterr is not. */
int64_t tk_csv_lasterrkind_w(void) {
    const char *m = csv_lasterrkind();
    return (int64_t)(intptr_t)(m ? m : "ok");
}

/* csv.lasterrline() -> u64 — 1-based physical line, 0 when not applicable. */
int64_t tk_csv_lasterrline_w(void) {
    return (int64_t)csv_lasterrline();
}
