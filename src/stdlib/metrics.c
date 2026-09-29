/*
 * metrics.c — in-process request counters and their Prometheus rendering.
 *
 * Epic 66: Prometheus metrics, histograms, upstream latency, cache metrics.
 * (Request IDs and W3C/B3 tracing were removed unused in 121.43.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Access / error log writers: removed (121.41) ──────────────────────
 *
 * metrics_log_access, metrics_log_error and their configuration setters
 * (log format, target, template, access/error log paths) wrote the request
 * path, User-Agent and Referer into the log raw -- a CR/LF or quote in any
 * of them forged log lines or JSON fields -- and metrics_log_error took a
 * caller format string.  Nothing called any of them: no stdlib module, no
 * glue wrapper, no .tki entry.  They were deleted rather than fixed; access
 * logging lives in log.c, whose writers escape every field.  Git history holds
 * the code (story 121.41).
 *
 * metrics_reopen_logs, left empty then, is gone too: its one caller,
 * server_ops.c's uncalled proc_reopen_logs, was removed by 121.49 (1), and
 * nothing else named it (121.51 (4)). */

/* ── Request IDs, W3C Trace Context and B3 tracing: removed (121.43) ───
 *
 * metrics_gen_request_id, metrics_parse_traceparent, metrics_new_trace,
 * metrics_build_traceparent and metrics_parse_b3 (66.1.4, 66.3.1, 66.3.2)
 * had no caller: no stdlib module, no glue wrapper, no .tki entry, nothing in
 * toke-ooke.  They read /dev/urandom through stdio rather than the runtime's
 * tk_random_bytes, and parse_traceparent copied the trace and parent ids
 * without checking they were hex.  Deleted rather than fixed, the way 121.41
 * removed this file's log writers; git history holds the code.  What remains
 * is the counter set metrics_prometheus renders (C133, C208). */

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
    /* 135.3j: was `volatile double duration_sum`, which nothing wrote and
     * nothing exported, so the histogram had no _sum and a request's latency
     * reached only the buckets.  Integer microseconds, updated lock-free,
     * exactly as upstream_sum_us below (135.3f). */
    volatile uint64_t duration_sum_us;
    volatile uint64_t duration_count;
    /* Upstream latency (66.2.4).  135.3f: the sum is kept in integer
     * MICROseconds so it can be updated with the same lock-free
     * __sync_fetch_and_add as every other field (there is no atomic add on a
     * double); it is rendered in seconds.  It used to be a `double` that
     * nothing ever wrote, so the latency a caller passed in was discarded. */
    volatile uint64_t upstream_sum_us;
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

    if (latency_ms > 0.0 && latency_ms < 1.0e12) {
        uint64_t us = (uint64_t)(latency_ms * 1000.0 + 0.5);
        __sync_fetch_and_add(&g_metrics.duration_sum_us, us);
    }
    __sync_fetch_and_add(&g_metrics.duration_count, 1);
}

/* 135.3f: this used to increment the counter and never read latency_ms, so a
 * caller instrumenting upstream latency got a request COUNT and believed it
 * had timings.  The latency is now accumulated and exported as
 * http_upstream_latency_seconds_{sum,count}.  A negative or non-finite
 * latency is a caller bug; it is counted but contributes nothing to the sum
 * rather than wrapping the unsigned accumulator. */
void metrics_record_upstream(double latency_ms)
{
    if (latency_ms > 0.0 && latency_ms < 1.0e12) {
        uint64_t us = (uint64_t)(latency_ms * 1000.0 + 0.5);
        __sync_fetch_and_add(&g_metrics.upstream_sum_us, us);
    }
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
        "http_request_duration_seconds_sum %llu.%06llu\n"
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
        (unsigned long long)(g_metrics.duration_sum_us / 1000000u),
        (unsigned long long)(g_metrics.duration_sum_us % 1000000u),
        (unsigned long long)(g_metrics.duration_count));

    /* Cache metrics (66.2.5) */
    PM("http_cache_hits_total", "Cache hits", g_metrics.cache_hits);
    PM("http_cache_misses_total", "Cache misses", g_metrics.cache_misses);
    PM("http_cache_stale_total", "Stale cache serves", g_metrics.cache_stale);

    /* Upstream latency (66.2.4) */
    off += snprintf(buf + off, cap - (size_t)off,
        "# HELP http_upstream_requests_total Upstream requests\n"
        "# TYPE http_upstream_requests_total counter\n"
        "http_upstream_requests_total %llu\n"
        "# HELP http_upstream_latency_seconds Upstream latency\n"
        "# TYPE http_upstream_latency_seconds summary\n"
        "http_upstream_latency_seconds_sum %llu.%06llu\n"
        "http_upstream_latency_seconds_count %llu\n",
        (unsigned long long)g_metrics.upstream_count,
        (unsigned long long)(g_metrics.upstream_sum_us / 1000000u),
        (unsigned long long)(g_metrics.upstream_sum_us % 1000000u),
        (unsigned long long)g_metrics.upstream_count);

    #undef PM

    /* The text is fixed-shape and well under cap, but a truncated exposition
     * is a wrong answer, not a short one: refuse rather than return it. */
    if (off < 0 || (size_t)off >= cap) { free(buf); return NULL; }
    return buf;
}
