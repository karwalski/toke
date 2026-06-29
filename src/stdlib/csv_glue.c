/*
 * csv_glue.c — i64-ABI wrappers for std.csv module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.csv.
 */

#include "csv.h"
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
    int64_t *outer = (int64_t *)malloc((nrows + 1) * sizeof(int64_t));
    if (!outer) { free(rows); return 0; }
    outer[0] = (int64_t)nrows;
    for (uint64_t i = 0; i < nrows; i++) {
        uint64_t fl = rows[i].len;
        int64_t *fields = (int64_t *)malloc((fl + 1) * sizeof(int64_t));
        if (!fields) { outer[i + 1] = 0; continue; }
        fields[0] = (int64_t)fl;
        for (uint64_t j = 0; j < fl; j++)
            fields[j + 1] = (int64_t)(intptr_t)rows[i].data[j];
        int64_t *cr = (int64_t *)malloc(sizeof(int64_t));
        if (!cr) { outer[i + 1] = 0; continue; }
        cr[0] = (int64_t)(intptr_t)(fields + 1);
        outer[i + 1] = (int64_t)(intptr_t)cr;
    }
    free(rows);
    return (int64_t)(intptr_t)(outer + 1);
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
    int64_t *blk = (int64_t *)malloc((a.len + 1) * sizeof(int64_t));
    if (!blk) return 0;
    blk[0] = (int64_t)a.len;
    for (uint64_t i = 0; i < a.len; i++)
        blk[i + 1] = (int64_t)(intptr_t)a.data[i];
    return (int64_t)(intptr_t)(blk + 1);
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
    if (!csv_reader_has_next(r)) return 0;
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
