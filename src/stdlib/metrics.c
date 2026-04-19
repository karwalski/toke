/*
 * metrics.c — Observability, metrics, and distributed tracing for std.http.
 *
 * Epic 66: access log formats, error log separation, container logging,
 * request IDs, Prometheus metrics, histograms, upstream latency,
 * cache metrics, W3C Trace Context, B3 tracing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdarg.h>

/* ── Log format types (66.1.1) ───────────────────────────────────────── */

typedef enum {
    LOG_FORMAT_COMBINED,  /* Apache combined */
    LOG_FORMAT_JSON,      /* JSON structured */
    LOG_FORMAT_CUSTOM,    /* custom template */
} LogFormat;

typedef enum {
    LOG_TARGET_FILE,
    LOG_TARGET_STDOUT,
    LOG_TARGET_STDERR,
} LogTarget;

static LogFormat g_log_format = LOG_FORMAT_COMBINED;
static LogTarget g_log_target = LOG_TARGET_STDERR;
static char     *g_log_template = NULL;
static FILE     *g_access_log = NULL;
static FILE     *g_error_log = NULL;
static char     *g_access_log_path = NULL;
static char     *g_error_log_path = NULL;

void metrics_set_log_format(LogFormat fmt) { g_log_format = fmt; }

void metrics_set_log_target(LogTarget target) { g_log_target = target; }

void metrics_set_log_template(const char *tmpl)
{
    free(g_log_template);
    g_log_template = tmpl ? strdup(tmpl) : NULL;
}

/* ── Log output helpers (66.1.3) ─────────────────────────────────────── */

static FILE *log_get_output(void)
{
    switch (g_log_target) {
    case LOG_TARGET_STDOUT: return stdout;
    case LOG_TARGET_STDERR: return stderr;
    case LOG_TARGET_FILE:   return g_access_log ? g_access_log : stderr;
    }
    return stderr;
}

void metrics_set_access_log(const char *path)
{
    if (g_access_log && g_access_log != stdout && g_access_log != stderr)
        fclose(g_access_log);
    free(g_access_log_path);
    g_access_log_path = path ? strdup(path) : NULL;
    g_access_log = path ? fopen(path, "a") : NULL;
}

/* Error log separation (66.1.2) */
void metrics_set_error_log(const char *path)
{
    if (g_error_log && g_error_log != stdout && g_error_log != stderr)
        fclose(g_error_log);
    free(g_error_log_path);
    g_error_log_path = path ? strdup(path) : NULL;
    g_error_log = path ? fopen(path, "a") : NULL;
}

void metrics_log_error(const char *fmt, ...)
{
    FILE *out = g_error_log ? g_error_log : stderr;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}

/* ── Access log formatting (66.1.1) ──────────────────────────────────── */

void metrics_log_access(const char *ip, const char *method, const char *uri,
                         uint32_t status, size_t bytes, double latency_ms,
                         const char *referer, const char *user_agent,
                         const char *request_id, const char *trace_id)
{
    FILE *out = log_get_output();
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%d/%b/%Y:%H:%M:%S %z", tm);

    switch (g_log_format) {
    case LOG_FORMAT_COMBINED:
        fprintf(out, "%s - - [%s] \"%s %s HTTP/1.1\" %u %zu \"%.200s\" \"%.200s\"",
                ip ? ip : "-", timebuf,
                method ? method : "GET", uri ? uri : "/",
                status, bytes,
                referer ? referer : "-",
                user_agent ? user_agent : "-");
        if (request_id) fprintf(out, " rid=%s", request_id);
        if (trace_id) fprintf(out, " tid=%s", trace_id);
        fputc('\n', out);
        break;

    case LOG_FORMAT_JSON:
        fprintf(out,
                "{\"time\":\"%s\",\"ip\":\"%s\",\"method\":\"%s\","
                "\"uri\":\"%s\",\"status\":%u,\"bytes\":%zu,"
                "\"latency_ms\":%.2f",
                timebuf, ip ? ip : "", method ? method : "GET",
                uri ? uri : "/", status, bytes, latency_ms);
        if (referer) fprintf(out, ",\"referer\":\"%s\"", referer);
        if (user_agent) fprintf(out, ",\"user_agent\":\"%s\"", user_agent);
        if (request_id) fprintf(out, ",\"request_id\":\"%s\"", request_id);
        if (trace_id) fprintf(out, ",\"trace_id\":\"%s\"", trace_id);
        fprintf(out, "}\n");
        break;

    case LOG_FORMAT_CUSTOM:
        /* Simple template substitution — for now, fallback to combined */
        fprintf(out, "%s - - [%s] \"%s %s\" %u %zu\n",
                ip ? ip : "-", timebuf,
                method ? method : "GET", uri ? uri : "/",
                status, bytes);
        break;
    }

    fflush(out);

    /* Write to error log if 4xx/5xx (66.1.2) */
    if (status >= 400 && g_error_log) {
        fprintf(g_error_log, "[%s] %s %s %s %u %zu\n",
                timebuf, ip ? ip : "-",
                method ? method : "GET", uri ? uri : "/",
                status, bytes);
        fflush(g_error_log);
    }
}

/* Log rotation signal handler (67.1.5) — reopen log files */
void metrics_reopen_logs(void)
{
    if (g_access_log_path) {
        if (g_access_log) fclose(g_access_log);
        g_access_log = fopen(g_access_log_path, "a");
    }
    if (g_error_log_path) {
        if (g_error_log) fclose(g_error_log);
        g_error_log = fopen(g_error_log_path, "a");
    }
}

/* ── Request ID generation (66.1.4) ──────────────────────────────────── */

/* Generate a UUID v4 string. Caller owns returned string. */
char *metrics_gen_request_id(void)
{
    uint8_t bytes[16];
    /* Use random bytes */
    FILE *f = fopen("/dev/urandom", "r");
    if (!f) return NULL;
    size_t r = fread(bytes, 1, 16, f);
    fclose(f);
    if (r != 16) return NULL;

    /* Set version (4) and variant (10xx) bits */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    char *id = malloc(37);
    if (!id) return NULL;
    snprintf(id, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return id;
}

/* ── In-process metrics counters (66.2.1) ────────────────────────────── */

typedef struct {
    volatile uint64_t total_requests;
    volatile uint64_t status_2xx;
    volatile uint64_t status_3xx;
    volatile uint64_t status_4xx;
    volatile uint64_t status_5xx;
    volatile uint64_t active_connections;
    volatile uint64_t bytes_sent;
    volatile uint64_t bytes_received;
    volatile uint64_t cache_hits;
    volatile uint64_t cache_misses;
    volatile uint64_t cache_stale;
    /* Histogram buckets for request duration (66.2.3) */
    volatile uint64_t hist_1ms;
    volatile uint64_t hist_5ms;
    volatile uint64_t hist_10ms;
    volatile uint64_t hist_25ms;
    volatile uint64_t hist_50ms;
    volatile uint64_t hist_100ms;
    volatile uint64_t hist_250ms;
    volatile uint64_t hist_500ms;
    volatile uint64_t hist_1s;
    volatile uint64_t hist_5s;
    volatile uint64_t hist_10s;
    volatile uint64_t hist_inf;
    volatile double   duration_sum;
    volatile uint64_t duration_count;
    /* Upstream latency (66.2.4) */
    volatile double   upstream_sum;
    volatile uint64_t upstream_count;
} Metrics;

static Metrics g_metrics = {0};

void metrics_record_request(uint32_t status, size_t bytes, double latency_ms)
{
    __sync_fetch_and_add(&g_metrics.total_requests, 1);

    if (status >= 200 && status < 300) __sync_fetch_and_add(&g_metrics.status_2xx, 1);
    else if (status >= 300 && status < 400) __sync_fetch_and_add(&g_metrics.status_3xx, 1);
    else if (status >= 400 && status < 500) __sync_fetch_and_add(&g_metrics.status_4xx, 1);
    else if (status >= 500) __sync_fetch_and_add(&g_metrics.status_5xx, 1);

    __sync_fetch_and_add(&g_metrics.bytes_sent, bytes);

    /* Histogram (66.2.3) */
    if (latency_ms <= 1.0) __sync_fetch_and_add(&g_metrics.hist_1ms, 1);
    else if (latency_ms <= 5.0) __sync_fetch_and_add(&g_metrics.hist_5ms, 1);
    else if (latency_ms <= 10.0) __sync_fetch_and_add(&g_metrics.hist_10ms, 1);
    else if (latency_ms <= 25.0) __sync_fetch_and_add(&g_metrics.hist_25ms, 1);
    else if (latency_ms <= 50.0) __sync_fetch_and_add(&g_metrics.hist_50ms, 1);
    else if (latency_ms <= 100.0) __sync_fetch_and_add(&g_metrics.hist_100ms, 1);
    else if (latency_ms <= 250.0) __sync_fetch_and_add(&g_metrics.hist_250ms, 1);
    else if (latency_ms <= 500.0) __sync_fetch_and_add(&g_metrics.hist_500ms, 1);
    else if (latency_ms <= 1000.0) __sync_fetch_and_add(&g_metrics.hist_1s, 1);
    else if (latency_ms <= 5000.0) __sync_fetch_and_add(&g_metrics.hist_5s, 1);
    else if (latency_ms <= 10000.0) __sync_fetch_and_add(&g_metrics.hist_10s, 1);
    else __sync_fetch_and_add(&g_metrics.hist_inf, 1);

    __sync_fetch_and_add(&g_metrics.duration_count, 1);
}

void metrics_record_upstream(double latency_ms)
{
    __sync_fetch_and_add(&g_metrics.upstream_count, 1);
}

void metrics_record_cache(const char *result)
{
    if (strcmp(result, "HIT") == 0) __sync_fetch_and_add(&g_metrics.cache_hits, 1);
    else if (strcmp(result, "MISS") == 0) __sync_fetch_and_add(&g_metrics.cache_misses, 1);
    else __sync_fetch_and_add(&g_metrics.cache_stale, 1);
}

void metrics_conn_add(void) { __sync_fetch_and_add(&g_metrics.active_connections, 1); }
void metrics_conn_remove(void) { __sync_fetch_and_sub(&g_metrics.active_connections, 1); }

/* ── Prometheus metrics endpoint (66.2.2) ────────────────────────────── */

/* Render Prometheus exposition format. Caller owns returned string. */
char *metrics_prometheus(void)
{
    size_t cap = 8192;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int off = 0;

    #define PM(name, help, value) \
        off += snprintf(buf + off, cap - (size_t)off, \
            "# HELP " name " " help "\n# TYPE " name " counter\n" \
            name " %llu\n", (unsigned long long)(value))

    PM("http_requests_total", "Total HTTP requests", g_metrics.total_requests);
    PM("http_requests_2xx", "2xx responses", g_metrics.status_2xx);
    PM("http_requests_3xx", "3xx responses", g_metrics.status_3xx);
    PM("http_requests_4xx", "4xx responses", g_metrics.status_4xx);
    PM("http_requests_5xx", "5xx responses", g_metrics.status_5xx);

    off += snprintf(buf + off, cap - (size_t)off,
        "# HELP http_connections_active Active connections\n"
        "# TYPE http_connections_active gauge\n"
        "http_connections_active %llu\n",
        (unsigned long long)g_metrics.active_connections);

    PM("http_response_bytes_total", "Total bytes sent", g_metrics.bytes_sent);

    /* Histogram (66.2.3) */
    off += snprintf(buf + off, cap - (size_t)off,
        "# HELP http_request_duration_seconds Request duration\n"
        "# TYPE http_request_duration_seconds histogram\n"
        "http_request_duration_seconds_bucket{le=\"0.001\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"0.005\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"0.01\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"0.025\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"0.05\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"0.1\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"0.25\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"0.5\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"1\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"5\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"10\"} %llu\n"
        "http_request_duration_seconds_bucket{le=\"+Inf\"} %llu\n"
        "http_request_duration_seconds_count %llu\n",
        (unsigned long long)g_metrics.hist_1ms,
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms + g_metrics.hist_50ms),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms + g_metrics.hist_50ms + g_metrics.hist_100ms),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms + g_metrics.hist_50ms + g_metrics.hist_100ms + g_metrics.hist_250ms),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms + g_metrics.hist_50ms + g_metrics.hist_100ms + g_metrics.hist_250ms + g_metrics.hist_500ms),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms + g_metrics.hist_50ms + g_metrics.hist_100ms + g_metrics.hist_250ms + g_metrics.hist_500ms + g_metrics.hist_1s),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms + g_metrics.hist_50ms + g_metrics.hist_100ms + g_metrics.hist_250ms + g_metrics.hist_500ms + g_metrics.hist_1s + g_metrics.hist_5s),
        (unsigned long long)(g_metrics.hist_1ms + g_metrics.hist_5ms + g_metrics.hist_10ms + g_metrics.hist_25ms + g_metrics.hist_50ms + g_metrics.hist_100ms + g_metrics.hist_250ms + g_metrics.hist_500ms + g_metrics.hist_1s + g_metrics.hist_5s + g_metrics.hist_10s),
        (unsigned long long)(g_metrics.total_requests),
        (unsigned long long)(g_metrics.duration_count));

    /* Cache metrics (66.2.5) */
    PM("http_cache_hits_total", "Cache hits", g_metrics.cache_hits);
    PM("http_cache_misses_total", "Cache misses", g_metrics.cache_misses);
    PM("http_cache_stale_total", "Stale cache serves", g_metrics.cache_stale);

    /* Upstream latency (66.2.4) */
    off += snprintf(buf + off, cap - (size_t)off,
        "# HELP http_upstream_requests_total Upstream requests\n"
        "# TYPE http_upstream_requests_total counter\n"
        "http_upstream_requests_total %llu\n",
        (unsigned long long)g_metrics.upstream_count);

    #undef PM

    return buf;
}

/* ── W3C Trace Context (66.3.1) ──────────────────────────────────────── */

typedef struct {
    char trace_id[33];  /* 16 bytes hex = 32 chars + NUL */
    char span_id[17];   /* 8 bytes hex = 16 chars + NUL */
    char parent_id[17];
    int  sampled;
} TraceContext;

/* Parse traceparent header: version-trace_id-parent_id-flags */
int metrics_parse_traceparent(const char *header, TraceContext *ctx)
{
    if (!header || !ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    /* Format: 00-<32 hex trace-id>-<16 hex parent-id>-<2 hex flags> */
    if (strlen(header) < 55) return -1;
    if (header[2] != '-' || header[35] != '-' || header[52] != '-')
        return -1;

    memcpy(ctx->trace_id, header + 3, 32);
    ctx->trace_id[32] = '\0';
    memcpy(ctx->parent_id, header + 36, 16);
    ctx->parent_id[16] = '\0';
    ctx->sampled = (header[54] == '1');

    /* Generate new span ID */
    FILE *f = fopen("/dev/urandom", "r");
    if (f) {
        uint8_t bytes[8];
        if (fread(bytes, 1, 8, f) == 8) {
            snprintf(ctx->span_id, sizeof(ctx->span_id),
                     "%02x%02x%02x%02x%02x%02x%02x%02x",
                     bytes[0], bytes[1], bytes[2], bytes[3],
                     bytes[4], bytes[5], bytes[6], bytes[7]);
        }
        fclose(f);
    }

    return 0;
}

/* Generate a new trace context */
int metrics_new_trace(TraceContext *ctx)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    FILE *f = fopen("/dev/urandom", "r");
    if (!f) return -1;

    uint8_t bytes[24]; /* 16 for trace, 8 for span */
    if (fread(bytes, 1, 24, f) != 24) { fclose(f); return -1; }
    fclose(f);

    snprintf(ctx->trace_id, sizeof(ctx->trace_id),
             "%02x%02x%02x%02x%02x%02x%02x%02x"
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);

    snprintf(ctx->span_id, sizeof(ctx->span_id),
             "%02x%02x%02x%02x%02x%02x%02x%02x",
             bytes[16], bytes[17], bytes[18], bytes[19],
             bytes[20], bytes[21], bytes[22], bytes[23]);

    ctx->sampled = 1;
    return 0;
}

/* Build traceparent header value. Caller owns returned string. */
char *metrics_build_traceparent(const TraceContext *ctx)
{
    if (!ctx) return NULL;
    char *buf = malloc(64);
    if (!buf) return NULL;
    snprintf(buf, 64, "00-%s-%s-%02x",
             ctx->trace_id, ctx->span_id, ctx->sampled ? 1 : 0);
    return buf;
}

/* ── B3 trace support (66.3.2) ───────────────────────────────────────── */

int metrics_parse_b3(const char *trace_id_hdr, const char *span_id_hdr,
                      const char *parent_id_hdr, const char *sampled_hdr,
                      TraceContext *ctx)
{
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    if (trace_id_hdr) {
        strncpy(ctx->trace_id, trace_id_hdr, sizeof(ctx->trace_id) - 1);
    }
    if (span_id_hdr) {
        strncpy(ctx->parent_id, span_id_hdr, sizeof(ctx->parent_id) - 1);
    }
    if (parent_id_hdr) {
        /* parent_id from B3 becomes our reference */
    }
    if (sampled_hdr) {
        ctx->sampled = (sampled_hdr[0] == '1');
    }

    /* Generate our own span ID */
    FILE *f = fopen("/dev/urandom", "r");
    if (f) {
        uint8_t bytes[8];
        if (fread(bytes, 1, 8, f) == 8) {
            snprintf(ctx->span_id, sizeof(ctx->span_id),
                     "%02x%02x%02x%02x%02x%02x%02x%02x",
                     bytes[0], bytes[1], bytes[2], bytes[3],
                     bytes[4], bytes[5], bytes[6], bytes[7]);
        }
        fclose(f);
    }

    return 0;
}
