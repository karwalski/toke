/*
 * diag.c — Diagnostic formatter and emitter for the toke reference compiler.
 *
 * =========================================================================
 * Role in the compiler pipeline
 * =========================================================================
 * Every compiler phase (lexer, parser, name resolution, type checker,
 * codegen) reports errors and warnings through diag_emit() or
 * diag_emit_span().  This file owns the formatting and output of those
 * diagnostics.  It never decides *what* to diagnose — it only controls
 * *how* diagnostics appear.
 *
 * =========================================================================
 * Output formats
 * =========================================================================
 * Three formats are supported, selectable via diag_set_format():
 *
 *   JSON  (the default, whatever stdout is -- 123.12b)
 *     One JSON object per line on stderr.  Fields include schema_version,
 *     diagnostic_id (D0000..D9999), error_code (E/W + 4-digit code),
 *     severity, stage (derived from code range), message, file, position,
 *     span, and optional "fix" hint.
 *
 *   Text  (--diag-text only)
 *     Rust/Clang style: "severity[CODE]: message", " --> file:line:col",
 *     the source line, and a '^' under the column with '~' across the span.
 *     Until 123.12b this was the default whenever stdout was a TTY, so a
 *     tool or agent driving tkc through a pseudo-terminal got prose instead
 *     of records.  Diagnostics must be machine-readable at all times
 *     (AGENTS.md §3.1; workspace AGENTS.md §6), so text is now opt-in
 *     and JSON never depends on the terminal.
 *
 *   SARIF v2.1.0  (--diag-sarif flag)
 *     Diagnostics are buffered in memory (up to SARIF_MAX_DIAGS) and
 *     flushed as a complete SARIF JSON document to stdout at the end of
 *     compilation via diag_flush_sarif().
 *
 * =========================================================================
 * Fix hints
 * =========================================================================
 * Callers may pass key/value pairs after the message argument.  The only
 * recognised key is "fix"; its value is a human-readable suggestion that
 * appears in the JSON "fix" field or in a SARIF "fixes" array.
 *
 * =========================================================================
 * Error code ranges
 * =========================================================================
 *   1000–1999  lex          (E1xxx / W1xxx)
 *   2000–2999  parse        (E2xxx / W2xxx)
 *   3000–3999  name_resolution
 *   4000–4999  type_check
 *   5000–5999  arena_check
 *   6000–6999  ir_lower
 *   9000–9999  codegen
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"

/* ── Internal mode (extended to include SARIF) ─────────────────────── */
enum { DIAG_INT_JSON = 0, DIAG_INT_TEXT = 1, DIAG_INT_SARIF = 2 };

static int         s_int_mode    = DIAG_INT_JSON;
static DiagMode    s_mode        = DIAG_MODE_JSON;   /* legacy accessor */
static int         s_error_count = 0;
static int         s_warn_count  = 0;
static int         s_seq         = 0;
static int         s_suppress    = 0;  /* when nonzero, emit_* functions are no-ops */
static const char *s_source_file = "";
static const char *s_source_buf  = NULL;
static size_t      s_source_len  = 0;
/* 123.12c: keep-predicate and the offsets of counted errors. */
static DiagKeepFn s_keep     = NULL;
static void      *s_keep_ctx = NULL;
static int        s_err_off[DIAG_ERR_OFF_MAX];
static int        s_err_off_n = 0;
/* 135.3j (7): optional sink that replaces the stderr/SARIF writers. */
static DiagSinkFn s_sink     = NULL;
static void      *s_sink_ctx = NULL;

/* ── SARIF buffer ──────────────────────────────────────────────────── */
#define SARIF_MAX_DIAGS 4096

typedef struct {
    int  severity;   /* DIAG_ERROR or DIAG_WARNING */
    int  code;
    int  line;
    int  col;
    int  byte_offset;
    int  span_start;
    int  span_end;
    char message[512];
    char fix[256];
} SarifEntry;

static SarifEntry s_sarif_buf[SARIF_MAX_DIAGS];
static int        s_sarif_count = 0;

/* diag_set_mode — Legacy setter; maps DiagMode to the internal tri-state. */
void diag_set_mode(DiagMode mode) { s_mode = mode; s_int_mode = (mode == DIAG_MODE_TEXT) ? DIAG_INT_TEXT : DIAG_INT_JSON; }
/*
 * diag_set_format — Set the diagnostic output format (JSON, text, or SARIF).
 *
 * SARIF mode keeps s_mode as DIAG_MODE_JSON for legacy compatibility but
 * routes output through the SARIF buffer instead of per-line JSON.
 */
void diag_set_format(DiagFormat fmt)
{
    if (fmt == DIAG_FMT_SARIF) {
        s_int_mode = DIAG_INT_SARIF;
        s_mode     = DIAG_MODE_JSON;  /* legacy field — keep JSON-like for compatibility */
    } else {
        s_int_mode = (fmt == DIAG_FMT_JSON) ? DIAG_INT_JSON : DIAG_INT_TEXT;
        s_mode     = (fmt == DIAG_FMT_JSON) ? DIAG_MODE_JSON : DIAG_MODE_TEXT;
    }
}
/* ── Wrapper-buffer rebasing (127.186) ─────────────────────────────── */
enum { DIAG_REBASE_MAX = 16 };
typedef struct { int prefix; int base; int len; } DiagRebase;
static DiagRebase s_rebase[DIAG_REBASE_MAX];
static int        s_rebase_depth = 0;   /* may exceed MAX: extra pushes are no-ops */

/* Map an offset in the innermost wrapper to an absolute source offset. */
static int rebase_offset(int off)
{
    int d = s_rebase_depth < DIAG_REBASE_MAX ? s_rebase_depth : DIAG_REBASE_MAX;
    if (d <= 0) return off;
    const DiagRebase *r = &s_rebase[d - 1];
    int rel = off - r->prefix;
    if (rel < 0) rel = 0;
    if (rel > r->len) rel = r->len;
    return r->base + rel;
}

void diag_rebase_push(int wrap_prefix_len, int src_offset, int expr_len)
{
    if (s_rebase_depth < DIAG_REBASE_MAX) {
        DiagRebase r = { wrap_prefix_len, rebase_offset(src_offset), expr_len };
        s_rebase[s_rebase_depth] = r;
    }
    s_rebase_depth++;
}

void diag_rebase_pop(void) { if (s_rebase_depth > 0) s_rebase_depth--; }

/* Rewrite (offset, line, col) in place when a rebase is active. */
static void rebase_pos(int *off, int *line, int *col)
{
    if (s_rebase_depth <= 0) return;
    int o = rebase_offset(*off);
    *off = o;
    if (!s_source_buf || o < 0 || (size_t)o > s_source_len) return;
    int ln = 1; size_t ls = 0;
    for (size_t i = 0; i < (size_t)o; i++)
        if (s_source_buf[i] == '\n') { ln++; ls = i + 1; }
    *line = ln;
    *col  = (int)((size_t)o - ls) + 1;
}

/* diag_set_source_file — Record the path used in SARIF artifact locations. */
void diag_set_source_file(const char *path) { s_source_file = path ? path : ""; }

/* 137.13: codegen records which source file each emitted declaration came
 * from, so a later clang/ld failure can name it. */
const char *diag_get_source_file(void) { return s_source_file ? s_source_file : ""; }

/* diag_set_source — Store a pointer to the source text for line extraction. */
void diag_set_source(const char *src, size_t len) { s_source_buf = src; s_source_len = len; }

/*
 * extract_source_line — Copy the source line containing byte_offset into buf.
 *
 * Scans backward from byte_offset to find the start of the line, then forward
 * to find the end.  Copies at most (buf_size - 1) characters and
 * NUL-terminates.  Returns 0 on success, -1 if no source is available.
 *
 * 127.267: *out_len is the number of bytes copied.  The line is NOT a C
 * string: a raw NUL in a string literal is legal source (W1021 warns on it),
 * so every consumer must write the line by length.  Before this the JSON
 * source_line stopped at the NUL — the line of the very byte W1021 reports
 * was cut short just before it.
 *
 * 127.269: a line longer than the buffer is cut on a UTF-8 character
 * boundary and *truncated is set.  A cut at a fixed byte count could split a
 * multi-byte sequence, and the JSON record then carried invalid UTF-8.
 */
static int
extract_source_line(int byte_offset, int line, char *buf, size_t buf_size,
                    size_t *out_len, int *truncated)
{
    *out_len = 0;
    *truncated = 0;
    if (!s_source_buf || s_source_len == 0 || byte_offset < 0 || buf_size == 0) {
        if (buf_size > 0) buf[0] = '\0';
        return -1;
    }
    /* 127.181: a diagnostic with no position (line 0, offset 0 -- E9003
     * "clang invocation failed" is the case) has no source line.  Before this
     * it printed line 1 of whatever buffer was registered, which in the
     * multi-module driver had already been freed. */
    if (line <= 0 && byte_offset == 0) {
        if (buf_size > 0) buf[0] = '\0';
        return -1;
    }
    size_t off = (size_t)byte_offset;
    if (off > s_source_len) off = s_source_len;

    /* Find line start */
    size_t ls = off;
    while (ls > 0 && s_source_buf[ls - 1] != '\n') --ls;

    /* Find line end */
    size_t le = off;
    while (le < s_source_len && s_source_buf[le] != '\n' && s_source_buf[le] != '\r') ++le;

    size_t line_len = le - ls;
    if (line_len >= buf_size) {
        line_len = buf_size - 1;
        /* The first byte left out must not be a continuation byte
         * (10xxxxxx): if it is, the cut is inside a sequence, so the
         * sequence's lead and earlier continuation bytes go too.  At most
         * three steps -- a UTF-8 sequence is at most four bytes. */
        for (int k = 0; k < 3 && line_len > 0 &&
                        ((unsigned char)s_source_buf[ls + line_len] & 0xC0) == 0x80; k++)
            line_len--;
        *truncated = 1;
    }
    memcpy(buf, s_source_buf + ls, line_len);
    buf[line_len] = '\0';
    *out_len = line_len;
    return 0;
}

/* diag_error_count — Return the cumulative error count for this compilation. */
int  diag_error_count(void)       { return s_error_count; }

/*
 * diag_reset — Zero all counters and the sequence number.
 *
 * Called between compilation units if the compiler were ever to process
 * multiple files in one invocation (not yet the case).
 */
void diag_reset(void)
{
    s_error_count = 0;
    s_warn_count  = 0;
    s_seq         = 0;
    s_err_off_n   = 0;
}

/*
 * diag_suppress — while on, diagnostics are neither printed NOR counted.
 *
 * Callers use this around a SPECULATIVE parse: migrate.c re-parses candidate
 * rewrites to see whether they hold together, and parser.c (127.24) parses the
 * inside of a `"\(…)"` interpolation to hand the name resolver real AST nodes.
 * Such a parse is a question, not a compilation, and its failures belong to
 * nobody.
 *
 * update_counters() used to run BEFORE the suppression check, so a failed
 * speculative parse silently incremented s_error_count: the compiler printed
 * nothing and then exited 1 on the strength of an error it had decided not to
 * report. `io.println("v \(a=1)")` produced exit 1 with zero diagnostics.
 * Counting now happens after the check — a suppressed diagnostic did not
 * happen, so it cannot decide the exit status.
 */
void diag_suppress(int on) { s_suppress = on; }
void diag_reset_counts(void) { s_error_count = 0; s_warn_count = 0; s_err_off_n = 0; }

/* Extracted key/value fields from the variadic tail of diag_emit(). */
typedef struct {
    const char *fix;
    const char *expected;
    const char *got;
} DiagFields;

/* Extract "fix", "expected", and "got" values from variadic key/value tail.
 * Convention: alternating (key, value) pairs; NULL key OR NULL value
 * terminates the scan. */
static DiagFields
extract_fields(va_list ap)
{
    DiagFields f = { NULL, NULL, NULL };
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        const char *val = va_arg(ap, const char *);
        if      (strcmp(key, "fix")      == 0) f.fix      = val;
        else if (strcmp(key, "expected") == 0) f.expected = val;
        else if (strcmp(key, "got")      == 0) f.got      = val;
        if (val == NULL) break;
    }
    return f;
}

/*
 * json_escape — Write a JSON-escaped copy of s to fp.
 *
 * Handles the six mandatory JSON escapes plus control characters < U+0020.
 * Used by both the per-line JSON emitter and the SARIF flusher.
 */
static void
json_escape(FILE *fp, const char *s)
{
    for (; *s != '\0'; ++s) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  fputs("\\\"", fp);
        else if (c == '\\') fputs("\\\\", fp);
        else if (c == '\n') fputs("\\n",  fp);
        else if (c == '\r') fputs("\\r",  fp);
        else if (c == '\t') fputs("\\t",  fp);
        else if (c < 0x20)  fprintf(fp, "\\u%04x", (unsigned)c);
        else                fputc(c, fp);
    }
}

/*
 * json_escape_n — json_escape for a counted byte string that may hold NULs
 * (127.267: the source line).  Every byte below U+0020, and DEL, is written
 * as \u00XX, so a raw control byte in source reaches a consumer as an
 * escape instead of ending or corrupting the record.
 */
static void
json_escape_n(FILE *fp, const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if      (c == '"')  fputs("\\\"", fp);
        else if (c == '\\') fputs("\\\\", fp);
        else if (c == '\n') fputs("\\n",  fp);
        else if (c == '\r') fputs("\\r",  fp);
        else if (c == '\t') fputs("\\t",  fp);
        else if (c < 0x20 || c == 0x7f) fprintf(fp, "\\u%04x", (unsigned)c);
        else                fputc(c, fp);
    }
}

/* severity_str — Return "error" or "warning" for a DiagSeverity value. */
static const char *
severity_str(DiagSeverity sev)
{
    return (sev == DIAG_ERROR) ? "error" : "warning";
}

/*
 * phase_for_code — Map a numeric diagnostic code to its compiler stage name.
 *
 * The code ranges are defined by convention (see file header).  Returns a
 * static string used as the "stage" field in JSON output.
 */
static const char *
phase_for_code(int code, DiagSeverity sev)
{
    /* 127.223 item 4: W1001 (lossy cast) sits in the 1xxx range for
     * historical reasons but is raised by the type checker (types.c,
     * NODE_CAST_EXPR); reporting it as "lex" pointed a consumer filtering by
     * stage at the wrong pass.  The code is stable; only its stage is fixed.
     * The number is shared with E1001 (a lexer error), hence the severity. */
    if (code == 1001 && sev == DIAG_WARNING) return "type_check";
    if (code >= 1000 && code <= 1999) return "lex";
    if (code >= 2000 && code <= 2999) return "parse";
    if (code >= 3000 && code <= 3999) return "name_resolution";
    if (code >= 4000 && code <= 4999) return "type_check";
    if (code >= 5000 && code <= 5999) return "arena_check";
    if (code >= 6000 && code <= 6999) return "ir_lower";
    if (code >= 8000 && code <= 8999) return "ffi";
    if (code >= 9000 && code <= 9999) return "codegen";
    return "lex";
}

/*
 * emit_json — Write one diagnostic as a single-line JSON object to stderr.
 *
 * Fields: schema_version, diagnostic_id (D+seq), error_code (E/W+code),
 * severity, stage, message, file (empty — set by consumer), pos
 * (offset/line/col), span_start, span_end, and optional "fix" hint.
 */
static void
emit_json(DiagSeverity sev, int code,
          int byte_offset, int line, int col,
          int span_start, int span_end,
          const char *message, const DiagFields *fields)
{
    char diag_id[16], code_str[8];
    snprintf(diag_id,  sizeof(diag_id),  "D%04d", s_seq);
    snprintf(code_str, sizeof(code_str), "%c%04d",
             (sev == DIAG_ERROR) ? 'E' : 'W', code);

    fprintf(stderr,
            "{\"schema_version\":\"1.0\","
            "\"diagnostic_id\":\"%s\","
            "\"error_code\":\"%s\","
            "\"severity\":\"%s\","
            "\"stage\":\"%s\","
            "\"message\":\"",
            diag_id, code_str, severity_str(sev), phase_for_code(code, sev));
    json_escape(stderr, message);
    fprintf(stderr, "\",\"file\":\"");
    json_escape(stderr, s_source_file);
    fprintf(stderr,
            "\","
            "\"pos\":{\"offset\":%d,\"line\":%d,\"col\":%d},"
            "\"span_start\":%d,\"span_end\":%d,"
            "\"context\":[]",
            byte_offset, line, col,
            (span_start >= 0) ? span_start : byte_offset,
            (span_end   >= 0) ? span_end   : byte_offset);

    /* expected field */
    fputs(",\"expected\":\"", stderr);
    if (fields && fields->expected && fields->expected[0])
        json_escape(stderr, fields->expected);
    fputc('"', stderr);

    /* got field */
    fputs(",\"got\":\"", stderr);
    if (fields && fields->got && fields->got[0])
        json_escape(stderr, fields->got);
    fputc('"', stderr);

    /* fix field — present only when there is a fix (127.183).  The schema
     * does not require it (spec v0.3 Appendix A), docs/spec/errors.md records
     * "Fix field: absent" for the codes with none, AGENTS.md §3.1 says to
     * leave it absent when uncertain, and the lint emitter already omits it.
     * An empty string here used to read as "a fix whose text is empty" to any
     * consumer that tests for the key rather than its value. */
    if (fields && fields->fix && fields->fix[0]) {
        fputs(",\"fix\":\"", stderr);
        json_escape(stderr, fields->fix);
        fputc('"', stderr);
    }

    /* source_line — the text of the line where the diagnostic occurred */
    {
        char sl_buf[1024];
        size_t sl_len;
        int sl_cut;
        if (extract_source_line(byte_offset, line, sl_buf, sizeof(sl_buf), &sl_len, &sl_cut) == 0) {
            fputs(",\"source_line\":\"", stderr);
            json_escape_n(stderr, sl_buf, sl_len);
            fputc('"', stderr);
            /* 127.269: present only when the line was cut, like "fix". */
            if (sl_cut) fputs(",\"source_line_truncated\":true", stderr);
        } else {
            fputs(",\"source_line\":\"\"", stderr);
        }
    }

    fputs("}\n", stderr);
}

/*
 * emit_text — Write a Rust/Clang-style diagnostic to stderr.
 *
 * Example output:
 *   error[E1001]: invalid escape sequence '\q'
 *    --> source.tk:3:15
 *     |
 *   3 | let x = "\q";
 *     |           ^^ valid escapes: \n \t \r \\ \" \0 \xNN
 *     |
 */
static void
emit_text(DiagSeverity sev, int code, int byte_offset, int line, int col,
          int span_len, const char *message, const DiagFields *fields)
{
    fprintf(stderr, "%s[%c%04d]: %s\n",
            severity_str(sev),
            (sev == DIAG_ERROR) ? 'E' : 'W',
            code, message);
    fprintf(stderr, " --> %s:%d:%d\n", s_source_file[0] ? s_source_file : "<input>", line, col);

    char sl_buf[1024];
    size_t sl_len;
    int sl_cut;
    if (extract_source_line(byte_offset, line, sl_buf, sizeof(sl_buf), &sl_len, &sl_cut) == 0) {
        /* Compute gutter width for the line number */
        char line_str[16];
        snprintf(line_str, sizeof(line_str), "%d", line);
        int gutter = (int)strlen(line_str);

        /* Blank gutter line */
        fprintf(stderr, "%*s |\n", gutter, "");

        /* Source line with line number.  127.267: written by length, a raw
         * control byte (tab aside) shown as '?' -- one column per byte, so
         * the caret below still lines up, and a NUL no longer ends it. */
        fprintf(stderr, "%d | ", line);
        for (size_t i = 0; i < sl_len; i++) {
            unsigned char c = (unsigned char)sl_buf[i];
            fputc((c < 0x20 && c != '\t') || c == 0x7f ? '?' : (int)c, stderr);
        }
        if (sl_cut) fputs(" ...", stderr);   /* 127.269: the line goes on */
        fputc('\n', stderr);

        /* Caret line: gutter + " | " + spaces up to col, then caret(s) */
        fprintf(stderr, "%*s | ", gutter, "");
        for (int i = 1; i < col; i++) fputc(' ', stderr);
        fputc('^', stderr);
        /* 123.12b: underline the rest of the token span with '~', clamped to
         * the visible source line so a bad/multi-line span can't run away. */
        if (span_len > 1) {
            int avail = (int)sl_len - (col - 1) - 1; /* chars after the caret */
            int under = span_len - 1;
            if (under > avail) under = avail;
            for (int i = 0; i < under; i++) fputc('~', stderr);
        }
        /* If there's a fix hint, show it after the caret */
        if (fields && fields->fix && fields->fix[0])
            fprintf(stderr, " %s", fields->fix);
        fputc('\n', stderr);

        /* Trailing blank gutter */
        fprintf(stderr, "%*s |\n", gutter, "");
    }
}

void diag_set_keep(DiagKeepFn keep, void *ctx) { s_keep = keep; s_keep_ctx = ctx; }
void diag_set_sink(DiagSinkFn sink, void *ctx) { s_sink = sink; s_sink_ctx = ctx; }

int diag_error_offsets(const int **out, int *out_count)
{
    if (out) *out = s_err_off;
    if (out_count) *out_count = s_err_off_n;
    return s_err_off_n == s_error_count;
}

/* update_counters — Bump the error or warning counter and the sequence id. */
static void
update_counters(DiagSeverity sev, int byte_offset)
{
    if (sev == DIAG_ERROR) {
        if (s_err_off_n == s_error_count && s_err_off_n < DIAG_ERR_OFF_MAX)
            s_err_off[s_err_off_n++] = byte_offset;
        ++s_error_count;
    } else {
        ++s_warn_count;
    }
    ++s_seq;
}

/*
 * buffer_sarif — Store a diagnostic in the in-memory SARIF ring buffer.
 *
 * Diagnostics are accumulated here and flushed as a complete SARIF v2.1.0
 * JSON document by diag_flush_sarif() at the end of compilation.  If the
 * buffer is full (SARIF_MAX_DIAGS reached) the diagnostic is silently
 * dropped.
 */
static void
buffer_sarif(DiagSeverity sev, int code,
             int byte_offset, int line, int col,
             int span_start, int span_end,
             const char *message, const char *fix)
{
    if (s_sarif_count >= SARIF_MAX_DIAGS) return;
    SarifEntry *e = &s_sarif_buf[s_sarif_count++];
    e->severity   = (int)sev;
    e->code       = code;
    e->line       = line;
    e->col        = col;
    e->byte_offset = byte_offset;
    e->span_start = span_start;
    e->span_end   = span_end;
    strncpy(e->message, message, sizeof(e->message) - 1);
    e->message[sizeof(e->message) - 1] = '\0';
    if (fix && fix[0]) {
        strncpy(e->fix, fix, sizeof(e->fix) - 1);
        e->fix[sizeof(e->fix) - 1] = '\0';
    } else {
        e->fix[0] = '\0';
    }
}

void
diag_emit(DiagSeverity sev, int code,
          int byte_offset, int line, int col,
          const char *message, ...)
{
    va_list ap;
    va_start(ap, message);
    DiagFields fields = extract_fields(ap);
    va_end(ap);

    if (s_suppress) return;
    rebase_pos(&byte_offset, &line, &col);
    if (s_keep && !s_keep(sev, code, byte_offset, s_keep_ctx)) return;
    update_counters(sev, byte_offset);
    if (s_sink)
        s_sink(sev, code, byte_offset, line, col, message, fields.fix, s_sink_ctx);
    else if (s_int_mode == DIAG_INT_TEXT)
        emit_text(sev, code, byte_offset, line, col, 0, message, &fields);
    else if (s_int_mode == DIAG_INT_SARIF)
        buffer_sarif(sev, code, byte_offset, line, col, -1, -1, message, fields.fix);
    else
        emit_json(sev, code, byte_offset, line, col, -1, -1, message, &fields);
}

void
diag_emit_span(DiagSeverity sev, int code,
               int byte_offset, int line, int col,
               int span_len, const char *message, ...)
{
    va_list ap;
    va_start(ap, message);
    DiagFields fields = extract_fields(ap);
    va_end(ap);

    if (s_suppress) return;
    rebase_pos(&byte_offset, &line, &col);
    if (s_keep && !s_keep(sev, code, byte_offset, s_keep_ctx)) return;
    update_counters(sev, byte_offset);
    if (s_sink)
        s_sink(sev, code, byte_offset, line, col, message, fields.fix, s_sink_ctx);
    else if (s_int_mode == DIAG_INT_TEXT)
        emit_text(sev, code, byte_offset, line, col, span_len, message, &fields);
    else if (s_int_mode == DIAG_INT_SARIF)
        buffer_sarif(sev, code, byte_offset, line, col,
                     byte_offset, byte_offset + span_len, message, fields.fix);
    else
        emit_json(sev, code, byte_offset, line, col,
                  byte_offset, byte_offset + span_len, message, &fields);
}

/* ── SARIF v2.1.0 flush ───────────────────────────────────────────── */

static void
sarif_json_escape(FILE *fp, const char *s)
{
    for (; *s != '\0'; ++s) {
        unsigned char c = (unsigned char)*s;
        if      (c == '"')  fputs("\\\"", fp);
        else if (c == '\\') fputs("\\\\", fp);
        else if (c == '\n') fputs("\\n",  fp);
        else if (c == '\r') fputs("\\r",  fp);
        else if (c == '\t') fputs("\\t",  fp);
        else if (c < 0x20)  fprintf(fp, "\\u%04x", (unsigned)c);
        else                fputc(c, fp);
    }
}

static const char *
sarif_level(int sev)
{
    return (sev == DIAG_ERROR) ? "error" : "warning";
}

void
diag_flush_sarif(void)
{
    if (s_int_mode != DIAG_INT_SARIF) return;

    FILE *fp = stdout;

    fputs("{\n"
          "  \"version\": \"2.1.0\",\n"
          "  \"$schema\": \"https://docs.oasis-open.org/sarif/sarif/v2.1.0/cos02/schemas/sarif-schema-2.1.0.json\",\n"
          "  \"runs\": [\n"
          "    {\n"
          "      \"tool\": {\n"
          "        \"driver\": {\n"
          "          \"name\": \"toke\",\n"
          "          \"version\": \"0.3.1\"\n"
          "        }\n"
          "      },\n"
          "      \"results\": [\n", fp);

    for (int i = 0; i < s_sarif_count; i++) {
        SarifEntry *e = &s_sarif_buf[i];
        char rule_id[16];
        snprintf(rule_id, sizeof(rule_id), "%c%04d",
                 (e->severity == DIAG_ERROR) ? 'E' : 'W', e->code);

        fprintf(fp,
                "        {\n"
                "          \"ruleId\": \"%s\",\n"
                "          \"level\": \"%s\",\n"
                "          \"message\": {\n"
                "            \"text\": \"",
                rule_id, sarif_level(e->severity));
        sarif_json_escape(fp, e->message);
        fputs("\"\n"
              "          },\n"
              "          \"locations\": [\n"
              "            {\n"
              "              \"physicalLocation\": {\n"
              "                \"artifactLocation\": {\n"
              "                  \"uri\": \"", fp);
        sarif_json_escape(fp, s_source_file);
        fprintf(fp,
                "\"\n"
                "                },\n"
                "                \"region\": {\n"
                "                  \"startLine\": %d,\n"
                "                  \"startColumn\": %d,\n"
                "                  \"byteOffset\": %d",
                e->line, e->col, e->byte_offset);
        if (e->span_start >= 0 && e->span_end > e->span_start) {
            fprintf(fp,
                    ",\n"
                    "                  \"byteLength\": %d",
                    e->span_end - e->span_start);
        }
        fputs("\n"
              "                }\n"
              "              }\n"
              "            }\n"
              "          ]", fp);

        if (e->fix[0]) {
            fputs(",\n"
                  "          \"fixes\": [\n"
                  "            {\n"
                  "              \"description\": {\n"
                  "                \"text\": \"", fp);
            sarif_json_escape(fp, e->fix);
            fputs("\"\n"
                  "              }\n"
                  "            }\n"
                  "          ]", fp);
        }

        fprintf(fp, "\n        }%s\n", (i + 1 < s_sarif_count) ? "," : "");
    }

    fputs("      ]\n"
          "    }\n"
          "  ]\n"
          "}\n", fp);
}
