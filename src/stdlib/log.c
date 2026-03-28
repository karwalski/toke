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

static int emit(int msg_level, const char *level_str,
                const char *msg, const char **fields, int field_count)
{
    ensure_level();
    if (msg_level < g_level) return 0;

    /* Timestamp */
    char ts_buf[32];
    current_iso8601(ts_buf, sizeof(ts_buf));

    /* Escaped message — allocate generous buffer */
    size_t msg_esc_size = msg ? (strlen(msg) * 6 + 8) : 8;
    char *msg_esc = malloc(msg_esc_size);
    if (!msg_esc) return 0;
    json_escape(msg_esc, msg_esc_size, msg);

    /* Begin JSON line */
    fprintf(stderr, "{\"level\":\"%s\",\"msg\":\"%s\",\"fields\":{",
            level_str, msg_esc);
    free(msg_esc);

    /* Fields: flat array ["k1","v1","k2","v2",...] */
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

        if (i > 0) fprintf(stderr, ",");
        fprintf(stderr, "\"%s\":\"%s\"", k_esc, v_esc);

        free(k_esc);
        free(v_esc);
    }

    fprintf(stderr, "},\"ts\":\"%s\"}\n", ts_buf);
    return 1;
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
