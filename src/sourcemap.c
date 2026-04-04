/*
 * sourcemap.c — compact-to-expanded span mapping for dual-view review.
 *
 * Provides a simple array-backed source map that records how positions in
 * compact (canonical) toke output correspond to positions in expanded
 * (pretty-printed) output.  The JSON serialisation follows the format:
 *
 *   {"version":1,"mappings":[
 *     {"compact":{"line":1,"col":0},"expanded":{"line":1,"col":0},"len":5},
 *     ...
 *   ]}
 *
 * Story: 10.8.3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sourcemap.h"

#define INITIAL_CAP 256

/* sourcemap_init — Allocate initial storage for span mappings. */
void sourcemap_init(SourceMap *sm)
{
    sm->count   = 0;
    sm->cap     = INITIAL_CAP;
    sm->entries = malloc((size_t)sm->cap * sizeof(SpanMapping));
    if (!sm->entries) sm->cap = 0;
}

/* sourcemap_add — Append a mapping entry, growing the array if needed. */
int sourcemap_add(SourceMap *sm,
                  int compact_line, int compact_col,
                  int expanded_line, int expanded_col,
                  int length)
{
    if (sm->count >= sm->cap) {
        int nc = sm->cap ? sm->cap * 2 : INITIAL_CAP;
        SpanMapping *ne = realloc(sm->entries, (size_t)nc * sizeof(SpanMapping));
        if (!ne) return -1;
        sm->entries = ne;
        sm->cap     = nc;
    }
    SpanMapping *e = &sm->entries[sm->count++];
    e->compact_line  = compact_line;
    e->compact_col   = compact_col;
    e->expanded_line = expanded_line;
    e->expanded_col  = expanded_col;
    e->length        = length;
    return 0;
}

/* sourcemap_lookup — Find the expanded position for a compact (line, col). */
int sourcemap_lookup(const SourceMap *sm,
                     int compact_line, int compact_col,
                     int *out_expanded_line, int *out_expanded_col)
{
    for (int i = 0; i < sm->count; i++) {
        const SpanMapping *e = &sm->entries[i];
        if (e->compact_line == compact_line &&
            compact_col >= e->compact_col &&
            compact_col < e->compact_col + e->length) {
            *out_expanded_line = e->expanded_line;
            *out_expanded_col  = e->expanded_col + (compact_col - e->compact_col);
            return 0;
        }
    }
    return -1;
}

/* sourcemap_emit_json — Write the source map as JSON to a file. */
int sourcemap_emit_json(const SourceMap *sm, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "{\"version\":1,\"mappings\":[");
    for (int i = 0; i < sm->count; i++) {
        const SpanMapping *e = &sm->entries[i];
        if (i > 0) fputc(',', f);
        fprintf(f,
            "{\"compact\":{\"line\":%d,\"col\":%d},"
            "\"expanded\":{\"line\":%d,\"col\":%d},"
            "\"len\":%d}",
            e->compact_line, e->compact_col,
            e->expanded_line, e->expanded_col,
            e->length);
    }
    fprintf(f, "]}");

    if (fclose(f) != 0) return -1;
    return 0;
}

/* sourcemap_free — Release the entries array. */
void sourcemap_free(SourceMap *sm)
{
    free(sm->entries);
    sm->entries = NULL;
    sm->count   = 0;
    sm->cap     = 0;
}
