/*
 * sourcemap.h — compact-to-expanded span mapping for dual-view review.
 *
 * Maps positions in compact (canonical) toke source to their corresponding
 * positions in expanded (pretty-printed) output.  Used with --sourcemap to
 * emit a .map file alongside formatted output.
 *
 * Story: 10.8.3
 */

#ifndef TKC_SOURCEMAP_H
#define TKC_SOURCEMAP_H

/* A single span mapping: one token/range in compact maps to one in expanded. */
typedef struct {
    int compact_line;   /* 1-based line in compact output  */
    int compact_col;    /* 0-based column in compact output */
    int expanded_line;  /* 1-based line in expanded output  */
    int expanded_col;   /* 0-based column in expanded output */
    int length;         /* length of the span in bytes       */
} SpanMapping;

/* Source map holding an array of span mappings. */
typedef struct {
    SpanMapping *entries;
    int          count;
    int          cap;
} SourceMap;

/* Initialise a source map (zero entries, preallocated capacity). */
void sourcemap_init(SourceMap *sm);

/* Add a mapping entry.  Returns 0 on success, -1 on allocation failure. */
int sourcemap_add(SourceMap *sm,
                  int compact_line, int compact_col,
                  int expanded_line, int expanded_col,
                  int length);

/* Look up the expanded position for a compact (line, col).
 * Returns 0 and fills out_expanded_line / out_expanded_col on match,
 * or -1 if no mapping covers that position.
 */
int sourcemap_lookup(const SourceMap *sm,
                     int compact_line, int compact_col,
                     int *out_expanded_line, int *out_expanded_col);

/* Emit the source map as JSON to the given file path.
 * Format: {"version":1,"mappings":[...]}
 * Returns 0 on success, -1 on I/O error.
 */
int sourcemap_emit_json(const SourceMap *sm, const char *path);

/* Free internal storage (entries array).  The SourceMap itself is not freed. */
void sourcemap_free(SourceMap *sm);

#endif /* TKC_SOURCEMAP_H */
