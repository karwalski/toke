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
 *   JSON  (default when stdout is not a TTY)
 *     One JSON object per line on stderr.  Fields include schema_version,
 *     diagnostic_id (D0000..D9999), error_code (E/W + 4-digit code),
 *     severity, stage (derived from code range), message, file, position,
 *     span, and optional "fix" hint.
 *
 *   Text  (default when stdout is a TTY)
 *     Human-readable two-line format: "severity[CODE]: message" followed
 *     by "  --> line N, col N".
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
static const char *s_source_file = "";

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
/* diag_set_source_file — Record the path used in SARIF artifact locations. */
void diag_set_source_file(const char *path) { s_source_file = path ? path : ""; }

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
}

/* Extract "fix" value from variadic key/value tail.
 * Convention: alternating (key, value) pairs; NULL key OR NULL value
 * terminates the scan.  Only "fix" key is extracted; others are skipped. */
static const char *
extract_fix(va_list ap)
{
    const char *key;
    while ((key = va_arg(ap, const char *)) != NULL) {
        const char *val = va_arg(ap, const char *);
        if (strcmp(key, "fix") == 0) return val;  /* val may be NULL */
        if (val == NULL) break;
    }
    return NULL;
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
phase_for_code(int code)
{
    if (code >= 1000 && code <= 1999) return "lex";
    if (code >= 2000 && code <= 2999) return "parse";
    if (code >= 3000 && code <= 3999) return "name_resolution";
    if (code >= 4000 && code <= 4999) return "type_check";
    if (code >= 5000 && code <= 5999) return "arena_check";
    if (code >= 6000 && code <= 6999) return "ir_lower";
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
          const char *message, const char *fix)
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
            diag_id, code_str, severity_str(sev), phase_for_code(code));
    json_escape(stderr, message);
    fprintf(stderr,
            "\",\"file\":\"\","
            "\"pos\":{\"offset\":%d,\"line\":%d,\"col\":%d},"
            "\"span_start\":%d,\"span_end\":%d,"
            "\"context\":[],\"expected\":\"\",\"got\":\"\"",
            byte_offset, line, col,
            (span_start >= 0) ? span_start : byte_offset,
            (span_end   >= 0) ? span_end   : byte_offset);

    if (fix != NULL && fix[0] != '\0') {
        fputs(",\"fix\":\"", stderr);
        json_escape(stderr, fix);
        fputc('"', stderr);
    }
    fputs("}\n", stderr);
}

/*
 * emit_text — Write a diagnostic in human-readable two-line format to stderr.
 *
 * Line 1: "severity[CODE]: message"
 * Line 2: "  --> line N, col N"
 */
static void
emit_text(DiagSeverity sev, int code, int line, int col, const char *message)
{
    fprintf(stderr, "%s[%c%04d]: %s\n",
            severity_str(sev),
            (sev == DIAG_ERROR) ? 'E' : 'W',
            code, message);
    fprintf(stderr, "  --> line %d, col %d\n", line, col);
}

/* update_counters — Bump the error or warning counter and the sequence id. */
static void
update_counters(DiagSeverity sev)
{
    if (sev == DIAG_ERROR) ++s_error_count;
    else                   ++s_warn_count;
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
    const char *fix = extract_fix(ap);
    va_end(ap);

    update_counters(sev);
    if (s_int_mode == DIAG_INT_TEXT)
        emit_text(sev, code, line, col, message);
    else if (s_int_mode == DIAG_INT_SARIF)
        buffer_sarif(sev, code, byte_offset, line, col, -1, -1, message, fix);
    else
        emit_json(sev, code, byte_offset, line, col, -1, -1, message, fix);
}

void
diag_emit_span(DiagSeverity sev, int code,
               int byte_offset, int line, int col,
               int span_len, const char *message, ...)
{
    va_list ap;
    va_start(ap, message);
    const char *fix = extract_fix(ap);
    va_end(ap);

    update_counters(sev);
    if (s_int_mode == DIAG_INT_TEXT)
        emit_text(sev, code, line, col, message);
    else if (s_int_mode == DIAG_INT_SARIF)
        buffer_sarif(sev, code, byte_offset, line, col,
                     byte_offset, byte_offset + span_len, message, fix);
    else
        emit_json(sev, code, byte_offset, line, col,
                  byte_offset, byte_offset + span_len, message, fix);
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
          "          \"name\": \"tkc\",\n"
          "          \"version\": \"0.1.0\"\n"
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
