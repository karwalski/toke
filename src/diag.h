#ifndef TK_DIAG_H
#define TK_DIAG_H

/*
 * diag.h — structured diagnostic emitter for the toke Phase 1 reference
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

/* Alternate format selector used by main.c (TEXT=0, JSON=1). */
typedef enum { DIAG_FMT_TEXT = 0, DIAG_FMT_JSON = 1 } DiagFormat;
void diag_set_format(DiagFormat fmt);

#endif /* TK_DIAG_H */
