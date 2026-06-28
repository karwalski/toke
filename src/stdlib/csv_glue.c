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
