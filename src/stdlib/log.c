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
/* 121.72: the path g_outfile was opened at, so tk_log_reopen_files can
 * reopen it after logrotate renames it (NULL when g_outfile is NULL). */
static char *g_outpath = NULL;

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

/* =========================================================================
 * 122.8 — sink-safe encoding, redaction, and the security-event stream
 *
 * ENCODING.  Every byte a caller supplies reaches a log sink through one of
 * three encoders, so no sink ever receives a raw control byte:
 *   enc_json      JSON string body: " \ and every byte < 0x20 escaped (\n \r
 *                 \t, else \u00XX), plus 0x7f.  Used by the JSON line format
 *                 and the NDJSON access log.
 *   enc_text      text-format fields, logfmt-style: a value that is empty or
 *                 holds a space, '=', '"', '\' or a control byte is written
 *                 in double quotes with \" \\ \n \r \t \xHH escapes; anything
 *                 else is written bare.  The message is the field msg=, by
 *                 the same rule (121.41 (3); before, it was written bare, so
 *                 " user=admin" inside a message spoofed a field).
 *   enc_clf       Combined Log Format, as Apache's ap_escape_logitem: " and \
 *                 backslash-escaped, control bytes as \n \r \t or \xHH; in
 *                 the unquoted positions (ip, method) a space is \x20 too.
 * Before 122.8 the text format wrote msg/key/value raw, the combined access
 * log wrote path/referer/user-agent raw inside its quotes, and the NDJSON
 * access log escaped only " and \: a request path or User-Agent carrying
 * "\r\n" wrote a second, forged line into every one of them.
 *
 * REDACTION.  A field whose KEY names a secret has its value replaced by
 * [REDACTED] in every format; bearer/basic credentials and JWTs are masked
 * wherever they appear in a message or value; and in the access log the
 * values of secret-named query parameters in the path and referer are
 * masked.  A key matches when, lower-cased with '_' '-' '.' removed, it
 * CONTAINS one of the configured names.  Defaults: password passwd secret
 * token apikey authorization cookie session credential privatekey.
 * TK_LOG_REDACT in the environment replaces the list (comma-separated) and
 * TK_LOG_REDACT=off disables key redaction; tk_log_redact_keys() does the
 * same from C.  Credential/JWT masking is always on.
 *
 * SECURITY STREAM.  tk_log_security(event, fields, n) writes one line with
 * level "security" and an "event" field.  It is never filtered by the log
 * level, and tk_log_security_set_output() can send it to its own file.
 * ---------------------------------------------------------------------- */

#include "strbuf.h"

#define TK_REDACTED "[REDACTED]"
#define TK_REDACT_MAX 32

static const char *k_redact_default[] = {
    "password", "passwd", "secret", "token", "apikey", "authorization",
    "cookie", "session", "credential", "privatekey", NULL
};
static char *g_redact_keys[TK_REDACT_MAX];  /* normalised; NULL-terminated */
static int   g_redact_init = 0;             /* 0 = not yet read from env  */
static int   g_redact_count = 0;

/* normalise a key for matching: lower-case, drop '_' '-' '.' */
static void redact_norm(TkStrBuf *b, const char *k)
{
    for (; k && *k; k++) {
        unsigned char c = (unsigned char)*k;
        if (c == '_' || c == '-' || c == '.') continue;
        strbuf_putc(b, (char)((c >= 'A' && c <= 'Z') ? c + 32 : c));
    }
}

static void redact_clear(void)
{
    for (int i = 0; i < g_redact_count; i++) free(g_redact_keys[i]);
    g_redact_count = 0;
    g_redact_keys[0] = NULL;
}

static void redact_add(const char *k, size_t n)
{
    if (g_redact_count >= TK_REDACT_MAX - 1 || n == 0) return;
    TkStrBuf raw = TK_STRBUF_INIT, nb = TK_STRBUF_INIT;
    strbuf_appendn(&raw, k, n);
    redact_norm(&nb, strbuf_cstr(&raw));
    strbuf_free(&raw);
    char *s = strbuf_take(&nb);
    if (!s) return;
    if (!*s) { free(s); return; }
    g_redact_keys[g_redact_count++] = s;
    g_redact_keys[g_redact_count] = NULL;
}

/* tk_log_redact_keys — replace the key list.  NULL restores the defaults;
 * "" or "off" disables key-based redaction; otherwise a comma-separated
 * list of names.  Returns the number of names now active. */
int tk_log_redact_keys(const char *csv)
{
    redact_clear();
    g_redact_init = 1;
    if (!csv) {
        for (int i = 0; k_redact_default[i]; i++)
            redact_add(k_redact_default[i], strlen(k_redact_default[i]));
        return g_redact_count;
    }
    if (strcmp(csv, "off") == 0) return 0;
    const char *p = csv;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        while (n && (*p == ' ')) { p++; n--; }
        while (n && p[n - 1] == ' ') n--;
        redact_add(p, n);
        if (!e) break;
        p = e + 1;
    }
    return g_redact_count;
}

static void redact_ensure(void)
{
    if (!g_redact_init) tk_log_redact_keys(getenv("TK_LOG_REDACT"));
}

static int redact_key_matches(const char *k)
{
    redact_ensure();
    if (!k || g_redact_count == 0) return 0;
    TkStrBuf nb = TK_STRBUF_INIT;
    redact_norm(&nb, k);
    int hit = 0;
    for (int i = 0; i < g_redact_count && !hit; i++)
        if (strstr(strbuf_cstr(&nb), g_redact_keys[i])) hit = 1;
    strbuf_free(&nb);
    return hit;
}

static int is_tokch(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '~' || c == '+' || c == '/' || c == '=';
}

static int is_b64url(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

/* length of a JWT (three non-empty base64url segments, the first starting
 * "eyJ") at s, or 0 */
static size_t jwt_len(const char *s)
{
    if (strncmp(s, "eyJ", 3) != 0) return 0;
    size_t i = 0;
    for (int seg = 0; seg < 3; seg++) {
        size_t st = i;
        while (is_b64url((unsigned char)s[i])) i++;
        if (i == st) return 0;
        if (seg < 2) { if (s[i] != '.') return 0; i++; }
    }
    return i;
}

/* redact_value — the value with credentials masked (malloc'd).  `mask_all`
 * replaces the whole value. */
static char *redact_value(const char *v, int mask_all)
{
    if (!v) return NULL;
    if (mask_all) return strdup(TK_REDACTED);
    TkStrBuf b = TK_STRBUF_INIT;
    const char *p = v;
    while (*p) {
        size_t jl;
        if ((strncmp(p, "Bearer ", 7) == 0 || strncmp(p, "bearer ", 7) == 0 ||
             strncmp(p, "Basic ", 6) == 0 || strncmp(p, "basic ", 6) == 0) &&
            (p == v || !is_tokch((unsigned char)p[-1]))) {
            size_t sp = (p[1] == 'e' || p[1] == 'E') ? 7 : 6;
            size_t t = 0;
            while (is_tokch((unsigned char)p[sp + t])) t++;
            if (t > 0) {
                strbuf_appendn(&b, p, sp);
                strbuf_append(&b, TK_REDACTED);
                p += sp + t;
                continue;
            }
        }
        if ((p == v || !is_b64url((unsigned char)p[-1])) && (jl = jwt_len(p)) > 0) {
            strbuf_append(&b, TK_REDACTED);
            p += jl;
            continue;
        }
        strbuf_putc(&b, *p++);
    }
    return strbuf_take(&b);
}

/* redact_query — mask the values of secret-named query parameters in a
 * request target or URL, then apply redact_value to the result. */
static char *redact_query(const char *target)
{
    if (!target) return NULL;
    const char *q = strchr(target, '?');
    if (!q) return redact_value(target, 0);
    TkStrBuf b = TK_STRBUF_INIT;
    strbuf_appendn(&b, target, (size_t)(q - target) + 1);
    const char *p = q + 1;
    while (*p) {
        const char *amp = strchr(p, '&');
        const char *end = amp ? amp : p + strlen(p);
        const char *eq = memchr(p, '=', (size_t)(end - p));
        if (eq) {
            TkStrBuf k = TK_STRBUF_INIT;
            strbuf_appendn(&k, p, (size_t)(eq - p));
            int hit = redact_key_matches(strbuf_cstr(&k));
            strbuf_free(&k);
            strbuf_appendn(&b, p, (size_t)(eq - p) + 1);
            if (hit) strbuf_append(&b, TK_REDACTED);
            else     strbuf_appendn(&b, eq + 1, (size_t)(end - eq - 1));
        } else {
            strbuf_appendn(&b, p, (size_t)(end - p));
        }
        if (!amp) break;
        strbuf_putc(&b, '&');
        p = amp + 1;
    }
    char *s = strbuf_take(&b);
    if (!s) return NULL;
    char *r = redact_value(s, 0);
    free(s);
    return r;
}

static void enc_json(TkStrBuf *b, const char *s)
{
    if (!s) { strbuf_append(b, "null"); return; }
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  strbuf_append(b, "\\\""); break;
        case '\\': strbuf_append(b, "\\\\"); break;
        case '\n': strbuf_append(b, "\\n");  break;
        case '\r': strbuf_append(b, "\\r");  break;
        case '\t': strbuf_append(b, "\\t");  break;
        default:
            if (*p < 0x20 || *p == 0x7f) strbuf_appendf(b, "\\u%04x", *p);
            else                         strbuf_putc(b, (char)*p);
        }
    }
}

/* control bytes, and backslash, escaped; `quote` also escapes '"'; `space`
 * also escapes ' ' (unquoted CLF positions). */
static void enc_ctl(TkStrBuf *b, const char *s, int quote, int space)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '\\')                 strbuf_append(b, "\\\\");
        else if (c == '"' && quote)    strbuf_append(b, "\\\"");
        else if (c == '\n')            strbuf_append(b, "\\n");
        else if (c == '\r')            strbuf_append(b, "\\r");
        else if (c == '\t')            strbuf_append(b, "\\t");
        else if (c < 0x20 || c == 0x7f || (c == ' ' && space))
                                       strbuf_appendf(b, "\\x%02x", c);
        else                           strbuf_putc(b, (char)c);
    }
}

/* logfmt value: bare when unambiguous, else quoted and escaped */
static void enc_text(TkStrBuf *b, const char *s)
{
    int q = (*s == '\0');
    for (const unsigned char *p = (const unsigned char *)s; *p && !q; p++)
        if (*p <= ' ' || *p == '=' || *p == '"' || *p == '\\' || *p == 0x7f) q = 1;
    if (!q) { strbuf_append(b, s); return; }
    strbuf_putc(b, '"');
    enc_ctl(b, s, 1, 0);
    strbuf_putc(b, '"');
}

static void enc_clf(TkStrBuf *b, const char *s, int quoted)
{
    if (!s) { strbuf_putc(b, '-'); return; }
    enc_ctl(b, s, 1, !quoted);
}

/* The security stream's own sink (NULL → the main log output). */
static FILE *g_secfile = NULL;
static char *g_secpath = NULL;   /* 121.72: g_secfile's path, for reopen */

/* 121.72: open `path` for append into *fp, remembering the path in *pathp;
 * NULL closes the sink.  Shared by tk_log_set_output and
 * tk_log_security_set_output. */
static int sink_open(FILE **fp, char **pathp, const char *path)
{
    if (*fp) { fclose(*fp); *fp = NULL; }
    free(*pathp); *pathp = NULL;
    if (!path) return 1;
    *fp = fopen(path, "a");
    if (!*fp) return 0;
    *pathp = strdup(path);   /* NULL on OOM: the sink works, is not reopened */
    return 1;
}

int tk_log_security_set_output(const char *path)
{
    return sink_open(&g_secfile, &g_secpath, path);
}

/* write one finished line with a single fwrite, so concurrent writers to the
 * same file never interleave inside a line */
static int write_line(FILE *out, TkStrBuf *b)
{
    if (b->oom) { strbuf_free(b); return 0; }
    fwrite(strbuf_cstr(b), 1, b->len, out);
    fflush(out);
    strbuf_free(b);
    return 1;
}

/* -------------------------------------------------------------------------
 * emit_json / emit_text — one line in the configured format.  `level_str` is
 * the level name; `event` (security stream only) adds an "event" field.
 * ---------------------------------------------------------------------- */

static int emit_json_to(FILE *out, const char *level_str, const char *event,
                        const char *msg, const char **fields, int field_count)
{
    char ts_buf[32];
    current_iso8601(ts_buf, sizeof(ts_buf));

    TkStrBuf b = TK_STRBUF_INIT;
    char *m = redact_value(msg, 0);
    strbuf_appendf(&b, "{\"level\":\"%s\",\"msg\":\"", level_str);
    enc_json(&b, msg ? m : NULL);
    strbuf_putc(&b, '"');
    free(m);
    if (event) {
        strbuf_append(&b, ",\"event\":\"");
        enc_json(&b, event);
        strbuf_putc(&b, '"');
    }

    /* Inline fields as top-level keys */
    int pairs = (field_count >= 2) ? (field_count / 2) : 0;
    for (int i = 0; i < pairs; i++) {
        const char *k = fields[i * 2];
        const char *v = fields[i * 2 + 1];
        char *rv = redact_value(v, redact_key_matches(k));
        strbuf_append(&b, ",\"");
        enc_json(&b, k);
        strbuf_append(&b, "\":\"");
        enc_json(&b, v ? rv : NULL);
        strbuf_putc(&b, '"');
        free(rv);
    }

    strbuf_appendf(&b, ",\"ts\":\"%s\"}\n", ts_buf);
    return write_line(out, &b);
}

static int emit_text_to(FILE *out, const char *level_str, const char *event,
                        const char *msg, const char **fields, int field_count)
{
    TkStrBuf b = TK_STRBUF_INIT;

    /* level string to an upper-case tag, e.g. "info" → "[INFO]" */
    strbuf_putc(&b, '[');
    for (const char *p = level_str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        strbuf_putc(&b, (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c);
    }
    strbuf_append(&b, "] ");

    /* 121.41 (3): the message is a logfmt field, msg=..., quoted by the same
     * rule as every value.  It used to be written bare, so a message holding
     * " user=admin" read as a user field it was not (owner decision
     * 2026-10-01 §3.2). */
    char *m = redact_value(msg ? msg : "", 0);
    strbuf_append(&b, "msg=");
    enc_text(&b, m ? m : "");
    free(m);
    if (event) {
        strbuf_append(&b, " event=");
        enc_text(&b, event);
    }

    int pairs = (field_count >= 2) ? (field_count / 2) : 0;
    for (int i = 0; i < pairs; i++) {
        const char *k = fields[i * 2];
        const char *v = fields[i * 2 + 1];
        char *rv = redact_value(v ? v : "", redact_key_matches(k));
        strbuf_putc(&b, ' ');
        enc_text(&b, k ? k : "");
        strbuf_putc(&b, '=');
        enc_text(&b, rv ? rv : "");
        free(rv);
    }

    strbuf_putc(&b, '\n');
    return write_line(out, &b);
}

static int emit_json(const char *level_str,
                     const char *msg, const char **fields, int field_count)
{
    return emit_json_to(output_stream(), level_str, NULL, msg, fields, field_count);
}

static int emit_text(const char *level_str,
                     const char *msg, const char **fields, int field_count)
{
    return emit_text_to(output_stream(), level_str, NULL, msg, fields, field_count);
}

/* tk_log_security — one security-event line; never filtered by level. */
int tk_log_security(const char *event, const char **fields, int field_count)
{
    FILE *out = g_secfile ? g_secfile : output_stream();
    const char *ev = event ? event : "unspecified";
    if (g_format == TK_LOG_FMT_TEXT)
        return emit_text_to(out, "security", ev, "security event", fields, field_count);
    return emit_json_to(out, "security", ev, "security event", fields, field_count);
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
    /* Closes any previous file; NULL resets output back to stderr. */
    return sink_open(&g_outfile, &g_outpath, path);
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

    /* 122.8: keys and values were copied into char[256]/char[512] stack
     * buffers and the copy loop simply STOPPED at the limit without
     * consuming the rest of the string, so the parser resumed mid-key and
     * read the remainder as further pairs -- a long key could forge fields.
     * Each string is now read whole into a TkStrBuf. */
    char *keys[MAX_CTX_PAIRS];
    char *vals[MAX_CTX_PAIRS];
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
        TkStrBuf kb = TK_STRBUF_INIT;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p + 1)) {
                p++; /* skip backslash, copy next char literally */
            }
            strbuf_putc(&kb, *p++);
        }
        if (*p == '"') p++; /* consume closing '"' */

        SKIP_WS(p);
        if (*p != ':') { strbuf_free(&kb); continue; } /* malformed; skip */
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
            strbuf_free(&kb);
            continue;
        }

        /* --- parse string value --- */
        p++; /* consume opening '"' */
        TkStrBuf vb = TK_STRBUF_INIT;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p + 1)) {
                p++; /* skip backslash, copy next char literally */
            }
            strbuf_putc(&vb, *p++);
        }
        if (*p == '"') p++; /* consume closing '"' */

        keys[pair_count] = strbuf_take(&kb);
        vals[pair_count] = strbuf_take(&vb);
        if (!keys[pair_count] || !vals[pair_count]) {
            free(keys[pair_count]); free(vals[pair_count]);
            break;
        }
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

    int rc = emit(TK_LOG_INFO, "info", msg, flat, pair_count * 2);
    for (int i = 0; i < pair_count; i++) { free(keys[i]); free(vals[i]); }
    return rc;
}

/* ==========================================================================
 * Access log with rotation (Story 47.1.1)
 * ========================================================================== */

#include <zlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>   /* flock — 127.191 */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const char *k_month_names[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/*
 * 127.191 — rotation is shared state, so it is decided under a lock and from
 * the FILE, never from a per-process counter.
 *
 * Three defects lost lines here, each on its own:
 *   1. the archive name had one-second resolution, so a second rotation in
 *      the same second renamed onto, and gzipped over, the first archive;
 *   2. a pre-fork server opens the log once and forks, so every worker held
 *      its own count and its own FILE*: when one rotated, the others went on
 *      writing into the renamed inode, which the gzip step then unlinked;
 *   3. the count started at 0 on every open, whatever the file already held.
 *
 * So every write now takes an exclusive flock on a sidecar lock file (the
 * live file cannot carry the lock, because rotation replaces its inode), and
 * under that lock: reopens the live file if its inode is not the one this
 * process holds; counts the newlines appended since this process last looked
 * (its own and every other writer's) — so the count is the shared file's,
 * and on first open it is the whole existing file's; writes; and rotates
 * when the count reaches max_lines.  Archive names carry microseconds and
 * the pid, and a name already taken (plain or .gz) is never reused.
 *
 * flock() locks belong to the open file description, which fork() SHARES, so
 * the lock fd is reopened in any process that did not open it (lock_pid).
 */
struct TkAccessLog {
    char *path;          /* full path to live log file */
    char *dir;           /* directory component          */
    char *stem;          /* filename stem (no extension) */
    char *ext;           /* extension incl. dot, or ""   */
    char *lock_path;     /* sidecar lock file (127.191)  */
    FILE *fp;
    int   lock_fd;       /* -1 = not open in this process */
    pid_t lock_pid;      /* process that opened lock_fd  */
    dev_t cur_dev;       /* identity of the inode fp holds */
    ino_t cur_ino;
    off_t counted_to;    /* bytes of that inode already counted */
    long  line_count;    /* newlines in that inode up to counted_to */
    int   max_lines;     /* 0 = no rotation              */
    int   max_files;     /* 0 = no limit                 */
    int   max_age_days;  /* 0 = disabled; >0 wins over max_files */
    int   is_stream;     /* 121.53: a tty/FIFO/device -- never reopened */
};

static TkAccessLog *g_access_log = NULL;
static TkAccessLog *g_error_log  = NULL;

/* ── Access log format flag (Story 47.1.3) ────────────────────────────── */

#define TK_ACCESS_FMT_COMBINED 0
#define TK_ACCESS_FMT_JSON     1

static int g_access_format = TK_ACCESS_FMT_COMBINED;

void tk_access_log_set_global(TkAccessLog *log) { g_access_log = log; }
TkAccessLog *tk_access_log_get_global(void)     { return g_access_log; }

void tk_error_log_set_global(TkAccessLog *log)  { g_error_log = log; }
TkAccessLog *tk_error_log_get_global(void)      { return g_error_log; }

/* ── Path splitting ───────────────────────────────────────────────────── */

static void split_path(const char *path,
                        char **dir_out, char **stem_out, char **ext_out)
{
    const char *slash = strrchr(path, '/');
    const char *filename = slash ? slash + 1 : path;

    if (slash) {
        size_t dlen = (size_t)(slash - path);
        *dir_out = malloc(dlen + 1);
        if (*dir_out) { memcpy(*dir_out, path, dlen); (*dir_out)[dlen] = '\0'; }
    } else {
        *dir_out = strdup(".");
    }

    const char *dot = strrchr(filename, '.');
    if (dot && dot != filename) {
        size_t slen = (size_t)(dot - filename);
        *stem_out = malloc(slen + 1);
        if (*stem_out) { memcpy(*stem_out, filename, slen); (*stem_out)[slen] = '\0'; }
        *ext_out = strdup(dot);
    } else {
        *stem_out = strdup(filename);
        *ext_out  = strdup("");
    }
}

/* ── Create directory (single level, ignore EEXIST) ───────────────────── */

static void mkdir_single(const char *dir)
{
    if (!dir || !dir[0] || strcmp(dir, ".") == 0) return;
#if defined(_WIN32)
    (void)mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

/* ── gzip compress src_path → dst_path, unlink src on success ─────────── */

static int compress_to_gz(const char *src_path, const char *dst_path)
{
    FILE *src = fopen(src_path, "rb");
    if (!src) return -1;

    gzFile gz = gzopen(dst_path, "wb9");
    if (!gz) { fclose(src); return -1; }

    char buf[65536];
    int ok = 1;
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
        if (gzwrite(gz, buf, (unsigned)n) == 0) { ok = 0; break; }
    }
    if (ferror(src)) ok = 0;

    fclose(src);
    /* 135.3j: gzclose() flushes the deflate stream's tail and the gzip
     * trailer -- for a small log, ALL of the compressed data -- and its
     * result was ignored.  A failed flush (disk full, EFBIG, EIO) still
     * unlinked the source, so rotation deleted the only complete copy of
     * the log and kept a truncated .gz.  Keep the source unless the .gz is
     * known to be whole. */
    if (gzclose(gz) != Z_OK) ok = 0;

    if (ok) {
        unlink(src_path);
    } else {
        unlink(dst_path);
    }
    return ok ? 0 : -1;
}

/* ── Retention: delete old rotated .gz files based on policy ───────────── */

static int str_ends_with(const char *s, const char *suffix)
{
    size_t slen   = strlen(s);
    size_t sflen  = strlen(suffix);
    if (sflen > slen) return 0;
    return strcmp(s + slen - sflen, suffix) == 0;
}

#define TK_ALOG_MAX_SCAN 4096

typedef struct { char path[1024]; time_t mtime; } AlogEntry;

static int alog_mtime_cmp(const void *a, const void *b)
{
    const AlogEntry *ea = (const AlogEntry *)a;
    const AlogEntry *eb = (const AlogEntry *)b;
    /* descending: newest first */
    if (eb->mtime > ea->mtime) return  1;
    if (eb->mtime < ea->mtime) return -1;
    return 0;
}

static void apply_retention(TkAccessLog *log)
{
    /* Build match prefix/suffix: "stem." and "ext.gz" */
    char prefix[512], suffix[128];
    snprintf(prefix, sizeof prefix, "%s.", log->stem);
    snprintf(suffix, sizeof suffix, "%s.gz", log->ext);

    AlogEntry *files = malloc(TK_ALOG_MAX_SCAN * sizeof *files);
    if (!files) return;
    int nfiles = 0;

    DIR *d = opendir(log->dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL && nfiles < TK_ALOG_MAX_SCAN) {
            const char *name = ent->d_name;
            if (strncmp(name, prefix, strlen(prefix)) != 0) continue;
            if (!str_ends_with(name, suffix))               continue;
            snprintf(files[nfiles].path, sizeof files[nfiles].path,
                     "%s/%s", log->dir, name);
            struct stat st;
            if (stat(files[nfiles].path, &st) != 0) continue;
            files[nfiles].mtime = st.st_mtime;
            nfiles++;
        }
        closedir(d);
    }

    if (log->max_age_days > 0) {
        time_t cutoff = time(NULL) - (time_t)log->max_age_days * 86400;
        for (int i = 0; i < nfiles; i++) {
            if (files[i].mtime < cutoff) unlink(files[i].path);
        }
    } else if (log->max_files > 0 && nfiles > log->max_files) {
        qsort(files, (size_t)nfiles, sizeof *files, alog_mtime_cmp);
        for (int i = log->max_files; i < nfiles; i++) {
            unlink(files[i].path);
        }
    }

    free(files);
}

/* ── 127.191: the lock, the inode check and the count ─────────────────── */

static int alog_lock(TkAccessLog *log)
{
    if (!log->lock_path) return -1;      /* stream mode: never locked */
    if (log->lock_fd >= 0 && log->lock_pid != getpid()) {
        /* Inherited across fork: the description (and so the lock) is shared
         * with the parent.  Drop our copy and open our own. */
        close(log->lock_fd);
        log->lock_fd = -1;
    }
    if (log->lock_fd < 0) {
        log->lock_fd = open(log->lock_path, O_RDWR | O_CREAT, 0644);
        if (log->lock_fd < 0) return -1;
        log->lock_pid = getpid();
    }
    while (flock(log->lock_fd, LOCK_EX) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static void alog_unlock(TkAccessLog *log)
{
    if (log->lock_fd >= 0) flock(log->lock_fd, LOCK_UN);
}

/* (Re)open the live file and forget the count: it belongs to the old inode. */
static void alog_reopen(TkAccessLog *log)
{
    if (log->fp) { fclose(log->fp); log->fp = NULL; }
    log->counted_to = 0;
    log->line_count = 0;
    log->cur_dev = 0;
    log->cur_ino = 0;
    /* "a+" so the newline count can pread() what other writers appended. */
    log->fp = fopen(log->path, "a+");
    if (!log->fp) return;
    setvbuf(log->fp, NULL, _IOLBF, 0);
    struct stat st;
    if (fstat(fileno(log->fp), &st) == 0) {
        log->cur_dev = st.st_dev;
        log->cur_ino = st.st_ino;
    }
}

/* Make fp the file now at log->path.  Called with the lock held. */
static void alog_sync_inode(TkAccessLog *log)
{
    struct stat ps;
    if (!log->fp || stat(log->path, &ps) != 0 ||
        ps.st_dev != log->cur_dev || ps.st_ino != log->cur_ino)
        alog_reopen(log);
}

/* Count the newlines appended since counted_to — by anyone.  Lock held. */
static void alog_count(TkAccessLog *log)
{
    if (!log->fp) return;
    struct stat st;
    if (fstat(fileno(log->fp), &st) != 0) return;
    if (st.st_size < log->counted_to) {          /* truncated under us */
        log->counted_to = 0;
        log->line_count = 0;
    }
    char buf[65536];
    while (log->counted_to < st.st_size) {
        off_t want = st.st_size - log->counted_to;
        size_t n = want > (off_t)sizeof buf ? sizeof buf : (size_t)want;
        ssize_t r = pread(fileno(log->fp), buf, n, log->counted_to);
        if (r <= 0) break;
        for (ssize_t k = 0; k < r; k++) if (buf[k] == '\n') log->line_count++;
        log->counted_to += r;
    }
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* ── Rotate: rename → reopen → (unlock) → gz → retention ──────────────── */

/* Called with the lock held; returns with it RELEASED.  Only the rename and
 * the reopen need the lock — once the live name points at a fresh inode no
 * other writer can reach the old one — so the gzip, which is proportional to
 * the file, does not stall every other worker. */
static void rotate_access_log_locked(TkAccessLog *log)
{
    if (log->fp) fflush(log->fp);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm = gmtime(&now);
    char ts[20];
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", tm);

    /* rotated path: dir/stem.YYYYMMDD-HHMMSS.uuuuuu-PID[-N].ext — microseconds
     * and the pid make a collision unlikely; the probe makes it impossible,
     * because every rotator holds the lock while it chooses. */
    char rotated[1024], gzpath[1100];
    for (int n = 0; ; n++) {
        if (n == 0)
            snprintf(rotated, sizeof rotated, "%s/%s.%s.%06ld-%ld%s", log->dir,
                     log->stem, ts, (long)tv.tv_usec, (long)getpid(), log->ext);
        else
            snprintf(rotated, sizeof rotated, "%s/%s.%s.%06ld-%ld-%d%s", log->dir,
                     log->stem, ts, (long)tv.tv_usec, (long)getpid(), n, log->ext);
        snprintf(gzpath, sizeof gzpath, "%s.gz", rotated);
        if (!path_exists(rotated) && !path_exists(gzpath)) break;
        if (n > 10000) { alog_unlock(log); return; }   /* give up: keep writing */
    }

    int moved = rename(log->path, rotated) == 0;
    alog_reopen(log);          /* creates the new live file */
    alog_unlock(log);

    if (moved) {
        compress_to_gz(rotated, gzpath);
        apply_retention(log);
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

TkAccessLog *tk_access_log_open(const char *path,
                                 int max_lines,
                                 int max_files,
                                 int max_age_days)
{
    if (!path || !path[0]) return NULL;

    TkAccessLog *log = calloc(1, sizeof *log);
    if (!log) return NULL;

    log->path         = strdup(path);
    log->max_lines    = max_lines;
    log->max_files    = max_files;
    log->max_age_days = max_age_days;
    log->lock_fd      = -1;

    /* A path that exists and is not a regular file (a tty, /dev/stderr, a
     * FIFO) cannot be rotated, counted or given a sidecar lock beside it --
     * "/dev/.stderr.lock" cannot be created, which made 127.191's open fail
     * for the stderr access log ooke uses by default.  Open it for plain
     * append; with no lock_path the writer never takes the lock and so
     * never rotates (tk_access_log_write's unlocked path). */
    struct stat pst;
    if (stat(path, &pst) == 0 && !S_ISREG(pst.st_mode)) {
        log->is_stream = 1;
        log->fp = fopen(path, "a");
        if (!log->fp) { tk_access_log_close(log); return NULL; }
        return log;
    }

    split_path(path, &log->dir, &log->stem, &log->ext);
    mkdir_single(log->dir);

    /* dir/.stem.ext.lock — hidden, and not matched by apply_retention (which
     * takes only stem.*ext.gz). */
    size_t ll = strlen(log->dir) + strlen(log->stem) + strlen(log->ext) + 16;
    log->lock_path = malloc(ll);
    if (!log->path || !log->lock_path) { tk_access_log_close(log); return NULL; }
    snprintf(log->lock_path, ll, "%s/.%s%s.lock", log->dir, log->stem, log->ext);

    if (alog_lock(log) != 0) {
        /* No sidecar lock can be made beside this path (/dev/stderr, even
         * when it resolves to a redirected regular file; a read-only dir).
         * A log that cannot rotate safely must still log: drop to plain
         * append with no lock_path, which never rotates. */
        free(log->lock_path);
        log->lock_path = NULL;
        log->fp = fopen(path, "a");
        if (!log->fp) { tk_access_log_close(log); return NULL; }
        return log;
    }
    alog_reopen(log);
    alog_count(log);           /* 127.191(3): the file's lines count */
    alog_unlock(log);
    if (!log->fp) { tk_access_log_close(log); return NULL; }

    return log;
}

/* 121.53 (1): reopen one log at its path -- see tk_log_reopen_files. */
static void alog_reopen_path(TkAccessLog *log)
{
    if (!log || !log->path) return;
    if (log->lock_path) {
        if (alog_lock(log) != 0) return;
        alog_reopen(log);
        alog_count(log);
        alog_unlock(log);
        return;
    }
    /* No lock: a stream (tty, FIFO, device), or a path beside which no lock
     * could be made -- /dev/stderr among them, which must not be reopened
     * (it may be a redirected write-only descriptor).  Any other path is a
     * regular file in a directory we could not write: reopen it at its
     * path.  alog_reopen is the same reopen the locked path uses; if it
     * cannot open the file, tk_access_log_write retries on its next line. */
    if (log->is_stream || strncmp(log->path, "/dev/", 5) == 0) return;
    alog_reopen(log);
}

/* 121.72: reopen a plain log sink (tk_log_set_output's or the security
 * stream's) at its path.  Same exclusions as alog_reopen_path: a /dev/ path
 * and anything at the path that is not a regular file (tty, FIFO) are left
 * alone.  The new file is opened before the old one is closed, and if it
 * cannot be opened the sink keeps writing where it was rather than losing
 * lines. */
static void sink_reopen(FILE **fp, const char *path)
{
    struct stat st;
    FILE *nf;
    if (!*fp || !path) return;
    if (strncmp(path, "/dev/", 5) == 0) return;
    if (stat(path, &st) == 0 && !S_ISREG(st.st_mode)) return;
    nf = fopen(path, "a");
    if (!nf) return;
    fclose(*fp);
    *fp = nf;
}

void tk_log_reopen_files(void)
{
    alog_reopen_path(g_access_log);
    alog_reopen_path(g_error_log);
    sink_reopen(&g_outfile, g_outpath);
    sink_reopen(&g_secfile, g_secpath);
}

/* ── Access log format selection (Story 47.1.3) ───────────────────────── */

int tk_access_log_set_format(const char *fmt)
{
    if (fmt && strcmp(fmt, "json") == 0) {
        g_access_format = TK_ACCESS_FMT_JSON;
        return 0;
    }
    if (fmt && strcmp(fmt, "combined") == 0) {
        g_access_format = TK_ACCESS_FMT_COMBINED;
        return 0;
    }
    /* 127.191: an unknown format used to be ignored silently, so a typo
     * ("jsno") left the log in combined format with nothing to say why. */
    /* 121.41 (3): the name is config-sourced; escape it like a log field so
     * it cannot write a forged line to stderr. */
    TkStrBuf b = TK_STRBUF_INIT;
    strbuf_append(&b, "log.accessformat: unknown format ");
    if (fmt) { strbuf_putc(&b, '"'); enc_ctl(&b, fmt, 1, 0); strbuf_putc(&b, '"'); }
    else     strbuf_append(&b, "(null)");
    strbuf_append(&b, " (expected \"combined\" or \"json\"); format unchanged\n");
    write_line(stderr, &b);
    return -1;
}

void tk_access_log_write(TkAccessLog *log,
                          const char *ip,
                          const char *method,
                          const char *path,
                          int         status,
                          size_t      bytes,
                          const char *referer,
                          const char *ua)
{
    if (!log) return;
    /* 127.191: decide against the shared file, under the lock. */
    int locked = alog_lock(log) == 0;
    if (locked) alog_sync_inode(log);
    else if (!log->fp) alog_reopen(log);
    if (!log->fp) { if (locked) alog_unlock(log); return; }

    /* 122.8: every caller-supplied field goes through an encoder (enc_json
     * or enc_clf) -- before, the combined format wrote path, referer and
     * user-agent raw and the NDJSON format escaped only '"' and '\\', so a
     * "\r\n" in a request path forged a second line -- and the path and
     * referer have secret-named query parameters masked.  The line is
     * built whole and written with one fwrite. */
    char *rpath = path ? redact_query(path) : NULL;
    char *rref  = referer ? redact_query(referer) : NULL;
    char *rua   = ua ? redact_value(ua, 0) : NULL;
    TkStrBuf b = TK_STRBUF_INIT;
    if (g_access_format == TK_ACCESS_FMT_JSON) {
        /* NDJSON format (Story 47.1.3) */
        char ts[32];
        current_iso8601(ts, sizeof ts);
        strbuf_appendf(&b, "{\"ts\":\"%s\",\"ip\":\"", ts);
        if (ip) enc_json(&b, ip); else strbuf_putc(&b, '-');
        strbuf_append(&b, "\",\"method\":\"");
        if (method) enc_json(&b, method); else strbuf_putc(&b, '-');
        strbuf_append(&b, "\",\"path\":\"");
        if (path) enc_json(&b, rpath ? rpath : ""); else strbuf_putc(&b, '-');
        strbuf_appendf(&b, "\",\"status\":%d,\"size\":%zu,\"ua\":\"", status, bytes);
        if (ua) enc_json(&b, rua ? rua : ""); else strbuf_putc(&b, '-');
        strbuf_append(&b, "\",\"ref\":\"");
        if (referer) enc_json(&b, rref ? rref : ""); else strbuf_putc(&b, '-');
        strbuf_append(&b, "\"}\n");
    } else {
        /* Combined Log Format timestamp: [06/Apr/2026:15:23:01 +0000] */
        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        char ts[40];
        snprintf(ts, sizeof ts, "%02d/%s/%04d:%02d:%02d:%02d +0000",
                 tm->tm_mday, k_month_names[tm->tm_mon], 1900 + tm->tm_year,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);

        enc_clf(&b, ip, 0);
        strbuf_appendf(&b, " - - [%s] \"", ts);
        enc_clf(&b, method, 0);
        strbuf_putc(&b, ' ');
        enc_clf(&b, path ? (rpath ? rpath : "") : NULL, 1);
        strbuf_appendf(&b, " HTTP/1.1\" %d ", status);
        if (bytes > 0) strbuf_appendf(&b, "%zu", bytes);
        else           strbuf_putc(&b, '-');
        strbuf_append(&b, " \"");
        enc_clf(&b, referer ? (rref ? rref : "") : NULL, 1);
        strbuf_append(&b, "\" \"");
        enc_clf(&b, ua ? (rua ? rua : "") : NULL, 1);
        strbuf_append(&b, "\"\n");
    }
    free(rpath); free(rref); free(rua);
    if (!b.oom) fwrite(strbuf_cstr(&b), 1, b.len, log->fp);
    strbuf_free(&b);
    fflush(log->fp);
    if (!locked) return;       /* no lock, no shared count: never rotate blind */

    alog_count(log);           /* this line and every other writer's */
    if (log->max_lines > 0 && log->line_count >= log->max_lines)
        rotate_access_log_locked(log);   /* releases the lock */
    else
        alog_unlock(log);
}

void tk_access_log_close(TkAccessLog *log)
{
    if (!log) return;
    if (log->fp) { fflush(log->fp); fclose(log->fp); }
    if (log->lock_fd >= 0) close(log->lock_fd);
    free(log->lock_path);
    free(log->path);
    free(log->dir);
    free(log->stem);
    free(log->ext);
    free(log);
}

/* ==========================================================================
 * Error log with rotation (Story 47.1.2)
 *
 * Reuses the TkAccessLog infrastructure — same struct, rotation, gzip,
 * and retention logic.  A separate global pointer (g_error_log) keeps
 * access and error streams independent.
 * ========================================================================== */

TkAccessLog *tk_error_log_open(const char *path,
                                int max_lines,
                                int max_files,
                                int max_age_days)
{
    /* Delegate to the same open logic used for access logs. */
    return tk_access_log_open(path, max_lines, max_files, max_age_days);
}

void tk_error_log_write(TkAccessLog *log,
                         const char *ip,
                         const char *method,
                         const char *path,
                         int         status,
                         size_t      bytes,
                         const char *referer,
                         const char *ua)
{
    /* Same Combined Log Format line, reusing the access-log writer. */
    tk_access_log_write(log, ip, method, path, status, bytes, referer, ua);
}

void tk_error_log_close(TkAccessLog *log)
{
    tk_access_log_close(log);
}
