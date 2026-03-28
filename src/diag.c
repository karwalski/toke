#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"

static DiagMode s_mode        = DIAG_MODE_JSON;
static int      s_error_count = 0;
static int      s_warn_count  = 0;
static int      s_seq         = 0;

void diag_set_mode(DiagMode mode) { s_mode = mode; }
void diag_set_format(DiagFormat fmt)
{
    s_mode = (fmt == DIAG_FMT_JSON) ? DIAG_MODE_JSON : DIAG_MODE_TEXT;
}
int  diag_error_count(void)       { return s_error_count; }

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

static const char *
severity_str(DiagSeverity sev)
{
    return (sev == DIAG_ERROR) ? "error" : "warning";
}

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
            "\"phase\":\"%s\","
            "\"message\":\"",
            diag_id, code_str, severity_str(sev), phase_for_code(code));
    json_escape(stderr, message);
    fprintf(stderr,
            "\",\"file\":\"\","
            "\"pos\":%d,\"line\":%d,\"column\":%d,"
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

static void
emit_text(DiagSeverity sev, int code, int line, int col, const char *message)
{
    fprintf(stderr, "%s[%c%04d]: %s\n",
            severity_str(sev),
            (sev == DIAG_ERROR) ? 'E' : 'W',
            code, message);
    fprintf(stderr, "  --> line %d, col %d\n", line, col);
}

static void
update_counters(DiagSeverity sev)
{
    if (sev == DIAG_ERROR) ++s_error_count;
    else                   ++s_warn_count;
    ++s_seq;
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
    if (s_mode == DIAG_MODE_TEXT)
        emit_text(sev, code, line, col, message);
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
    if (s_mode == DIAG_MODE_TEXT)
        emit_text(sev, code, line, col, message);
    else
        emit_json(sev, code, byte_offset, line, col,
                  byte_offset, byte_offset + span_len, message, fix);
}
