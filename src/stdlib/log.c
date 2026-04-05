/*
 * log.c — Implementation of the std.log standard library module.
 *
 * Emits newline-delimited JSON to stderr.  Output format:
 *   {"level":"info","msg":"...","fields":{"k1":"v1"},"ts":"<ISO8601>"}\n
 *
 * Log level is read once from TK_LOG_LEVEL on first use, then cached.
 * Levels (lowest to highest): DEBUG=0, INFO=1, WARN=2, ERROR=3.
 * Default when TK_LOG_LEVEL is absent or unrecognised: INFO (1).
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.
 *
 * Story: 3.8.1  Branch: feature/stdlib-3.8-log
 */

#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Level constants
 * ---------------------------------------------------------------------- */

#define TK_LOG_DEBUG 0
#define TK_LOG_INFO  1
#define TK_LOG_WARN  2
#define TK_LOG_ERROR 3

/* Sentinel: level has not been initialised yet. */
#define TK_LOG_UNINIT (-1)

static int g_level = TK_LOG_UNINIT;

/* -------------------------------------------------------------------------
 * Format constants — story 28.5.3
 * ---------------------------------------------------------------------- */

#define TK_LOG_FMT_JSON 0
#define TK_LOG_FMT_TEXT 1

static int g_format = TK_LOG_FMT_JSON; /* default: JSON */

/* -------------------------------------------------------------------------
 * Output destination — story 28.5.3
 *
 * NULL means write to stderr.  When non-NULL, this FILE* owns an open fd.
 * ---------------------------------------------------------------------- */

static FILE *g_outfile = NULL; /* NULL → use stderr */

/* Helper: return the active output stream. */
static FILE *output_stream(void)
{
    return (g_outfile != NULL) ? g_outfile : stderr;
}

/* -------------------------------------------------------------------------
 * Level initialisation
 * ---------------------------------------------------------------------- */

static int parse_level(const char *s)
{
    if (!s) return TK_LOG_INFO;
    if (strcmp(s, "DEBUG") == 0) return TK_LOG_DEBUG;
    if (strcmp(s, "INFO")  == 0) return TK_LOG_INFO;
    if (strcmp(s, "WARN")  == 0) return TK_LOG_WARN;
    if (strcmp(s, "ERROR") == 0) return TK_LOG_ERROR;
    return TK_LOG_INFO; /* unrecognised → default INFO */
}

static void ensure_level(void)
{
    if (g_level == TK_LOG_UNINIT) {
        g_level = parse_level(getenv("TK_LOG_LEVEL"));
    }
}

void tk_log_set_level(const char *level)
{
    int parsed = parse_level(level);
    /* parse_level returns INFO for unknown strings; only accept known ones. */
    if (level &&
        (strcmp(level, "DEBUG") == 0 || strcmp(level, "INFO")  == 0 ||
         strcmp(level, "WARN")  == 0 || strcmp(level, "ERROR") == 0)) {
        g_level = parsed;
    }
    /* If level is unrecognised, leave g_level as-is (or initialise). */
    if (g_level == TK_LOG_UNINIT) g_level = TK_LOG_INFO;
}

/* -------------------------------------------------------------------------
 * JSON escaping
 *
 * Writes the JSON-escaped form of src into dst (which must be large enough).
 * Returns the number of bytes written (not counting the NUL terminator).
 * ---------------------------------------------------------------------- */

static int json_escape(char *dst, size_t dst_size, const char *src)
{
    int written = 0;
    if (!src) {
        /* Write JSON null literal */
        if (dst_size >= 5) {
            memcpy(dst, "null", 4);
            dst[4] = '\0';
            return 4;
        }
        return 0;
    }
    for (const char *p = src; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* Ensure there is always room for the longest escape (6 bytes) + NUL */
        if ((size_t)written + 8 >= dst_size) break;
        switch (c) {
        case '"':  dst[written++] = '\\'; dst[written++] = '"';  break;
        case '\\': dst[written++] = '\\'; dst[written++] = '\\'; break;
        case '\n': dst[written++] = '\\'; dst[written++] = 'n';  break;
        case '\r': dst[written++] = '\\'; dst[written++] = 'r';  break;
        case '\t': dst[written++] = '\\'; dst[written++] = 't';  break;
        default:
            if (c < 0x20) {
                /* ASCII control character → \uXXXX */
                written += snprintf(dst + written,
                                    dst_size - (size_t)written,
                                    "\\u%04x", c);
            } else {
                dst[written++] = (char)c;
            }
            break;
        }
    }
    dst[written] = '\0';
    return written;
}

/* -------------------------------------------------------------------------
 * Timestamp helper
 *
 * Writes an ISO-8601 UTC timestamp with millisecond precision into buf.
 * e.g. "2026-03-28T12:34:56.789Z"
 * buf must be at least 32 bytes.
 * ---------------------------------------------------------------------- */

static void current_iso8601(char *buf, size_t buf_size)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        snprintf(buf, buf_size, "1970-01-01T00:00:00.000Z");
        return;
    }
    struct tm *tm_info = gmtime(&ts.tv_sec);
    if (!tm_info) {
        snprintf(buf, buf_size, "1970-01-01T00:00:00.000Z");
        return;
    }
    char date_buf[24];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%dT%H:%M:%S", tm_info);
    int ms = (int)(ts.tv_nsec / 1000000L);
    snprintf(buf, buf_size, "%s.%03dZ", date_buf, ms);
}

/* -------------------------------------------------------------------------
 * Core emit function
 * ---------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * emit_json — write a JSON-format log line
 * ---------------------------------------------------------------------- */

static int emit_json(const char *level_str,
                     const char *msg, const char **fields, int field_count)
{
    FILE *out = output_stream();

    /* Timestamp */
    char ts_buf[32];
    current_iso8601(ts_buf, sizeof(ts_buf));

    /* Escaped message */
    size_t msg_esc_size = msg ? (strlen(msg) * 6 + 8) : 8;
    char *msg_esc = malloc(msg_esc_size);
    if (!msg_esc) return 0;
    json_escape(msg_esc, msg_esc_size, msg);

    /* Begin JSON line: level and msg */
    fprintf(out, "{\"level\":\"%s\",\"msg\":\"%s\"", level_str, msg_esc);
    free(msg_esc);

    /* Inline fields as top-level keys */
    int pairs = (field_count >= 2) ? (field_count / 2) : 0;
    for (int i = 0; i < pairs; i++) {
        const char *k = fields[i * 2];
        const char *v = fields[i * 2 + 1];

        size_t k_esc_size = k ? (strlen(k) * 6 + 8) : 8;
        size_t v_esc_size = v ? (strlen(v) * 6 + 8) : 8;

        char *k_esc = malloc(k_esc_size);
        char *v_esc = malloc(v_esc_size);
        if (!k_esc || !v_esc) {
            free(k_esc);
            free(v_esc);
            break;
        }
        json_escape(k_esc, k_esc_size, k);
        json_escape(v_esc, v_esc_size, v);

        fprintf(out, ",\"%s\":\"%s\"", k_esc, v_esc);

        free(k_esc);
        free(v_esc);
    }

    fprintf(out, ",\"ts\":\"%s\"}\n", ts_buf);
    fflush(out);
    return 1;
}

/* -------------------------------------------------------------------------
 * emit_text — write a human-readable text log line
 * ---------------------------------------------------------------------- */

static int emit_text(const char *level_str,
                     const char *msg, const char **fields, int field_count)
{
    FILE *out = output_stream();

    /* Convert level string to uppercase tag, e.g. "info" → "[INFO]" */
    char tag[16];
    int ti = 0;
    tag[ti++] = '[';
    for (const char *p = level_str; *p && ti < 13; p++) {
        unsigned char c = (unsigned char)*p;
        tag[ti++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
    }
    tag[ti++] = ']';
    tag[ti]   = '\0';

    fprintf(out, "%s %s", tag, msg ? msg : "");

    int pairs = (field_count >= 2) ? (field_count / 2) : 0;
    for (int i = 0; i < pairs; i++) {
        const char *k = fields[i * 2];
        const char *v = fields[i * 2 + 1];
        fprintf(out, " %s=%s", k ? k : "", v ? v : "");
    }

    fprintf(out, "\n");
    fflush(out);
    return 1;
}

/* -------------------------------------------------------------------------
 * Core emit dispatcher
 * ---------------------------------------------------------------------- */

static int emit(int msg_level, const char *level_str,
                const char *msg, const char **fields, int field_count)
{
    ensure_level();
    if (msg_level < g_level) return 0;

    if (g_format == TK_LOG_FMT_TEXT) {
        return emit_text(level_str, msg, fields, field_count);
    }
    return emit_json(level_str, msg, fields, field_count);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

int tk_log_info(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_INFO, "info", msg, fields, field_count);
}

int tk_log_warn(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_WARN, "warn", msg, fields, field_count);
}

int tk_log_error(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_ERROR, "error", msg, fields, field_count);
}

/* -------------------------------------------------------------------------
 * Story 28.5.3 additions
 * ---------------------------------------------------------------------- */

int tk_log_debug(const char *msg, const char **fields, int field_count)
{
    return emit(TK_LOG_DEBUG, "debug", msg, fields, field_count);
}

void tk_log_set_format(const char *fmt)
{
    if (!fmt) return;
    if (strcmp(fmt, "json") == 0) {
        g_format = TK_LOG_FMT_JSON;
    } else if (strcmp(fmt, "text") == 0) {
        g_format = TK_LOG_FMT_TEXT;
    }
    /* Unknown fmt strings are silently ignored. */
}

int tk_log_set_output(const char *path)
{
    /* Close any previously opened output file. */
    if (g_outfile != NULL) {
        fclose(g_outfile);
        g_outfile = NULL;
    }
    if (path == NULL) {
        /* Caller is resetting back to stderr. */
        return 1;
    }
    g_outfile = fopen(path, "a");
    return (g_outfile != NULL) ? 1 : 0;
}

/*
 * tk_log_with_context — minimal hand-rolled JSON object parser.
 *
 * Parses a JSON object string of the form {"k":"v",...} and collects
 * string-valued key/value pairs, then emits them via tk_log_info.
 *
 * Limitations (intentional, no external deps):
 *   - Only top-level string-to-string pairs are extracted.
 *   - Unicode escape sequences in keys/values are passed through verbatim.
 *   - Nested objects/arrays are skipped.
 */
int tk_log_with_context(const char *msg, const char *context_json)
{
    /* Maximum number of context key/value pairs we support. */
#define MAX_CTX_PAIRS 32

    const char *keys[MAX_CTX_PAIRS];
    const char *vals[MAX_CTX_PAIRS];
    /* Buffers to hold the unescaped strings (generous sizes). */
    char key_bufs[MAX_CTX_PAIRS][256];
    char val_bufs[MAX_CTX_PAIRS][512];
    int pair_count = 0;

    if (!context_json) {
        return tk_log_info(msg, NULL, 0);
    }

    const char *p = context_json;

    /* Skip whitespace helper (inline). */
#define SKIP_WS(ptr) while (*(ptr) == ' ' || *(ptr) == '\t' || \
                            *(ptr) == '\n' || *(ptr) == '\r') (ptr)++

    SKIP_WS(p);
    if (*p != '{') {
        /* Not a JSON object; log without context fields. */
        return tk_log_info(msg, NULL, 0);
    }
    p++; /* consume '{' */

    while (*p && *p != '}' && pair_count < MAX_CTX_PAIRS) {
        SKIP_WS(p);
        if (*p != '"') { p++; continue; } /* skip non-string tokens */

        /* --- parse key --- */
        p++; /* consume opening '"' */
        int ki = 0;
        while (*p && *p != '"' && ki < 255) {
            if (*p == '\\' && *(p + 1)) {
                p++; /* skip backslash, copy next char literally */
            }
            key_bufs[pair_count][ki++] = *p++;
        }
        key_bufs[pair_count][ki] = '\0';
        if (*p == '"') p++; /* consume closing '"' */

        SKIP_WS(p);
        if (*p != ':') continue; /* malformed; skip */
        p++; /* consume ':' */
        SKIP_WS(p);

        /* Only handle string values; skip non-string values. */
        if (*p != '"') {
            /* Skip past the value token (number, bool, null, object, array). */
            int depth = 0;
            while (*p) {
                if (*p == '{' || *p == '[') depth++;
                else if (*p == '}' || *p == ']') {
                    if (depth == 0) break;
                    depth--;
                }
                else if ((*p == ',' || *p == '}') && depth == 0) break;
                p++;
            }
            if (*p == ',') p++;
            continue;
        }

        /* --- parse string value --- */
        p++; /* consume opening '"' */
        int vi = 0;
        while (*p && *p != '"' && vi < 511) {
            if (*p == '\\' && *(p + 1)) {
                p++; /* skip backslash, copy next char literally */
            }
            val_bufs[pair_count][vi++] = *p++;
        }
        val_bufs[pair_count][vi] = '\0';
        if (*p == '"') p++; /* consume closing '"' */

        keys[pair_count] = key_bufs[pair_count];
        vals[pair_count] = val_bufs[pair_count];
        pair_count++;

        SKIP_WS(p);
        if (*p == ',') p++; /* consume comma between pairs */
    }

#undef SKIP_WS
#undef MAX_CTX_PAIRS

    /* Build the flat fields array expected by emit. */
    const char *flat[64]; /* 2 * MAX_CTX_PAIRS */
    for (int i = 0; i < pair_count; i++) {
        flat[i * 2]     = keys[i];
        flat[i * 2 + 1] = vals[i];
    }

    return emit(TK_LOG_INFO, "info", msg, flat, pair_count * 2);
}
