/*
 * csv_glue.c — i64-ABI wrappers for std.csv module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.csv.
 */

#include "csv.h"
#include <stdint.h>
#include <stdlib.h>

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
    const char *s = (const char *)(intptr_t)data;
    uint64_t len = 0;
    while (s[len]) len++;
    uint64_t nrows = 0;
    StrArray *rows = csv_parse(s, len, &nrows);
    if (!rows || nrows == 0) return 0;
    /* Pack nrows + row pointer into a heap block so the runtime can access */
    int64_t *block = (int64_t *)malloc(sizeof(int64_t) + nrows * sizeof(StrArray));
    if (!block) { free(rows); return 0; }
    block[0] = (int64_t)nrows;
    StrArray *dest = (StrArray *)(block + 1);
    for (uint64_t i = 0; i < nrows; i++) dest[i] = rows[i];
    free(rows);
    return (int64_t)(intptr_t)block;
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
