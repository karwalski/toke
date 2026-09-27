#ifndef TK_DIAG_H
#define TK_DIAG_H

#include <stddef.h> /* size_t */

/*
 * diag.h — structured diagnostic emitter for the toke Profile 1 reference
 * compiler.
 *
 * Diagnostics are emitted to stderr as one JSON object per line (default) or
 * as human-readable text when DIAG_MODE_TEXT is active.  The JSON schema
 * conforms to toke-spec-v02.md Appendix B (schema_version "1.0").
 *
 * diag_emit() takes optional key/value string pairs after the message,
 * terminated by a NULL key (or a NULL value which also terminates):
 *
 *   diag_emit(DIAG_ERROR, E1001, off, line, col, "msg", "fix", "text", NULL);
 *   diag_emit(DIAG_ERROR, E1001, off, line, col, "msg", NULL);   // no fix
 */

/* -------------------------------------------------------------------------
 * Output mode
 * ------------------------------------------------------------------------- */
typedef enum {
    DIAG_MODE_JSON = 0,
    DIAG_MODE_TEXT = 1
} DiagMode;

void diag_set_mode(DiagMode mode);

/* -------------------------------------------------------------------------
 * Severity — must match the DIAG_SEVERITY_DEFINED guard used by parser.h.
 * ------------------------------------------------------------------------- */
#ifndef DIAG_SEVERITY_DEFINED
#define DIAG_SEVERITY_DEFINED
typedef enum { DIAG_WARNING = 0, DIAG_ERROR = 1 } DiagSeverity;
#endif

/* -------------------------------------------------------------------------
 * diag_emit — emit a diagnostic.
 *
 * Optional variadic tail: alternating (const char *key, const char *value)
 * pairs.  The list is terminated when a NULL key or NULL value is seen.
 * Currently only the "fix" key is recognised; all others are ignored.
 * ------------------------------------------------------------------------- */
void diag_emit(DiagSeverity severity, int code,
               int byte_offset, int line, int col,
               const char *message, ...);

/* Same as diag_emit() but also records span_start/span_end in the JSON. */
void diag_emit_span(DiagSeverity severity, int code,
                    int byte_offset, int line, int col,
                    int span_len, const char *message, ...);

int  diag_error_count(void);
void diag_reset(void);
void diag_suppress(int on);
void diag_reset_counts(void);

/* Alternate format selector used by main.c (TEXT=0, JSON=1, SARIF=2). */
typedef enum { DIAG_FMT_TEXT = 0, DIAG_FMT_JSON = 1, DIAG_FMT_SARIF = 2 } DiagFormat;
void diag_set_format(DiagFormat fmt);

/* Set the source filename for SARIF location reporting. */
void diag_set_source_file(const char *path);
/* The path last passed to diag_set_source_file ("" if none). */
const char *diag_get_source_file(void);

/* Store a pointer to the source text so diagnostics can include source lines.
 * The pointer must remain valid for the lifetime of the compilation unit. */
void diag_set_source(const char *src, size_t len);

/* Flush buffered SARIF output. Call before exit when DIAG_FMT_SARIF is active.
 * No-op in other modes. Writes the complete SARIF v2.1.0 envelope to stdout. */
void diag_flush_sarif(void);

/* 123.12c: cross-phase recovery support.
 *
 * diag_set_keep installs a predicate consulted for every diagnostic after
 * position rebasing: when it returns 0 the diagnostic is dropped -- neither
 * printed nor counted, exactly like diag_suppress.  NULL removes it.  main.c
 * uses it to run the type checker after name-resolution errors while keeping
 * only diagnostics located in declarations the name errors did not touch.
 *
 * diag_error_offsets returns the byte offsets (after rebasing) of the errors
 * counted so far, up to DIAG_ERR_OFF_MAX; *out_count is the number recorded
 * and the return value is 1 when every counted error was recorded, 0 when the
 * record overflowed (a caller must then assume an error it cannot place). */
typedef int (*DiagKeepFn)(DiagSeverity sev, int code, int byte_offset,
                          void *ctx);
void diag_set_keep(DiagKeepFn keep, void *ctx);
#define DIAG_ERR_OFF_MAX 64
int  diag_error_offsets(const int **out, int *out_count);

/* 127.186: rebase diagnostics raised while parsing a SYNTHETIC wrapper buffer
 * (the `m=i;f=e():$str{<EXPR;};` an interpolation segment is parsed inside)
 * onto the real source.  Between push and pop, an offset `o` in the wrapper
 * is reported at src_offset + clamp(o - wrap_prefix_len, 0, expr_len), with
 * line and col recomputed from the registered source.  src_offset is in the
 * coordinates of the ENCLOSING buffer: pushes nest (an interpolation inside
 * an interpolation) and each maps through the one below it. */
void diag_rebase_push(int wrap_prefix_len, int src_offset, int expr_len);
void diag_rebase_pop(void);

/* 135.3j (7): a diagnostic sink.  While one is set, every diagnostic that
 * survives suppression and the keep-predicate is counted as usual and handed
 * to the sink INSTEAD of being written to stderr (or the SARIF buffer).  The
 * WASM build uses it: a browser has no stderr to read, so tkc_check_source()
 * collects the records here.  `fix` is NULL when the diagnostic has none. */
typedef void (*DiagSinkFn)(DiagSeverity sev, int code, int byte_offset,
                           int line, int col, const char *message,
                           const char *fix, void *ctx);
void diag_set_sink(DiagSinkFn sink, void *ctx);

#endif /* TK_DIAG_H */
