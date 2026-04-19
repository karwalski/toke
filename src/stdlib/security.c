/*
 * security.c — Security hardening for std.http.
 *
 * Epic 65: rate limiting (per-IP, per-route), connection limits, Slowloris
 * defense, URI validation, body validation, SQL injection detection,
 * XSS detection, WAF rule engine, sub-request auth, client certs,
 * CSP header builder, per-route CORS.
 */

#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>

/* ── Token-bucket rate limiter (65.1.1) ──────────────────────────────── */

typedef struct {
    char    ip[64];
    double  tokens;
    time_t  last_check;
    int     active_conns; /* for 65.1.3 */
} RateBucket;

#define RATE_MAX_BUCKETS 4096

static RateBucket g_rate_buckets[RATE_MAX_BUCKETS];
static int        g_rate_bucket_count = 0;
static double     g_rate_limit_rps = 100.0;   /* requests per second */
static double     g_rate_burst = 200.0;        /* max burst size */
static time_t     g_rate_cleanup_interval = 60;
static time_t     g_rate_last_cleanup = 0;

void security_set_rate_limit(double requests_per_second, double burst)
{
    if (requests_per_second > 0) g_rate_limit_rps = requests_per_second;
    if (burst > 0) g_rate_burst = burst;
}

static RateBucket *rate_find_or_create(const char *ip)
{
    /* Cleanup old entries periodically */
    time_t now = time(NULL);
    if (now - g_rate_last_cleanup > g_rate_cleanup_interval) {
        int w = 0;
        for (int r = 0; r < g_rate_bucket_count; r++) {
            if (now - g_rate_buckets[r].last_check < 300) {
                if (w != r) g_rate_buckets[w] = g_rate_buckets[r];
                w++;
            }
        }
        g_rate_bucket_count = w;
        g_rate_last_cleanup = now;
    }

    for (int i = 0; i < g_rate_bucket_count; i++) {
        if (strcmp(g_rate_buckets[i].ip, ip) == 0) return &g_rate_buckets[i];
    }

    if (g_rate_bucket_count >= RATE_MAX_BUCKETS) return NULL;

    RateBucket *b = &g_rate_buckets[g_rate_bucket_count++];
    memset(b, 0, sizeof(*b));
    strncpy(b->ip, ip, sizeof(b->ip) - 1);
    b->tokens = g_rate_burst;
    b->last_check = now;
    return b;
}

/* Check if a request from this IP is allowed. Returns 1 if allowed. */
int security_rate_check(const char *ip)
{
    RateBucket *b = rate_find_or_create(ip);
    if (!b) return 1; /* allow if bucket table full */

    time_t now = time(NULL);
    double elapsed = difftime(now, b->last_check);
    b->last_check = now;

    /* Refill tokens */
    b->tokens += elapsed * g_rate_limit_rps;
    if (b->tokens > g_rate_burst) b->tokens = g_rate_burst;

    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        return 1;
    }
    return 0; /* rate limited */
}

/* Compute Retry-After header value in seconds */
int security_rate_retry_after(const char *ip)
{
    RateBucket *b = rate_find_or_create(ip);
    if (!b || b->tokens >= 1.0) return 0;
    double needed = 1.0 - b->tokens;
    int secs = (int)(needed / g_rate_limit_rps) + 1;
    return secs > 0 ? secs : 1;
}

/* ── Per-route rate limiting (65.1.2) ────────────────────────────────── */

#define MAX_ROUTE_LIMITS 64

typedef struct {
    char    *pattern;
    double   rps;
    double   burst;
} RouteRateLimit;

static RouteRateLimit g_route_limits[MAX_ROUTE_LIMITS];
static int            g_route_limit_count = 0;

void security_set_route_rate_limit(const char *pattern, double rps,
                                    double burst)
{
    if (g_route_limit_count >= MAX_ROUTE_LIMITS) return;
    RouteRateLimit *rl = &g_route_limits[g_route_limit_count++];
    rl->pattern = strdup(pattern);
    rl->rps = rps;
    rl->burst = burst > 0 ? burst : rps * 2;
}

/* Check per-route rate limit. Returns 1 if allowed. */
int security_route_rate_check(const char *ip, const char *path)
{
    for (int i = 0; i < g_route_limit_count; i++) {
        if (strncmp(path, g_route_limits[i].pattern,
                    strlen(g_route_limits[i].pattern)) == 0) {
            /* Use a composite key of ip+route for the bucket */
            char key[256];
            snprintf(key, sizeof(key), "%s|%s", ip,
                     g_route_limits[i].pattern);
            /* Temporarily adjust global rate and check */
            double saved_rps = g_rate_limit_rps;
            double saved_burst = g_rate_burst;
            g_rate_limit_rps = g_route_limits[i].rps;
            g_rate_burst = g_route_limits[i].burst;
            int ok = security_rate_check(key);
            g_rate_limit_rps = saved_rps;
            g_rate_burst = saved_burst;
            return ok;
        }
    }
    return security_rate_check(ip);
}

/* ── Per-IP connection limits (65.1.3) ───────────────────────────────── */

static int g_max_conns_per_ip = 100;

void security_set_max_conns_per_ip(int max_conns)
{
    if (max_conns > 0) g_max_conns_per_ip = max_conns;
}

int security_conn_add(const char *ip)
{
    RateBucket *b = rate_find_or_create(ip);
    if (!b) return 1;
    b->active_conns++;
    return (b->active_conns <= g_max_conns_per_ip);
}

void security_conn_remove(const char *ip)
{
    for (int i = 0; i < g_rate_bucket_count; i++) {
        if (strcmp(g_rate_buckets[i].ip, ip) == 0) {
            if (g_rate_buckets[i].active_conns > 0)
                g_rate_buckets[i].active_conns--;
            return;
        }
    }
}

/* ── Slowloris defense (65.1.4) ──────────────────────────────────────── */

static int g_min_header_rate = 200; /* bytes/second minimum for headers */
static int g_header_timeout = 10;   /* max seconds to receive all headers */

void security_set_slowloris_params(int min_rate, int timeout)
{
    if (min_rate > 0) g_min_header_rate = min_rate;
    if (timeout > 0) g_header_timeout = timeout;
}

int security_get_header_timeout(void) { return g_header_timeout; }
int security_get_min_header_rate(void) { return g_min_header_rate; }

/* ── URI validation (65.2.1) ─────────────────────────────────────────── */

static size_t g_max_uri_length = 8192;

void security_set_max_uri_length(size_t max_len)
{
    if (max_len > 0) g_max_uri_length = max_len;
}

/* Validate a URI. Returns 0 if valid, -1 if invalid. */
int security_validate_uri(const char *uri)
{
    if (!uri) return -1;
    size_t len = strlen(uri);

    /* Check length */
    if (len > g_max_uri_length) return -1;

    /* Check for null bytes */
    for (size_t i = 0; i < len; i++) {
        if (uri[i] == '\0') return -1;
        /* Check for non-printable characters (except allowed URL chars) */
        if ((unsigned char)uri[i] < 0x20 && uri[i] != '\t') return -1;
    }

    /* Validate percent-encoding */
    for (size_t i = 0; i < len; i++) {
        if (uri[i] == '%') {
            if (i + 2 >= len) return -1;
            if (!isxdigit((unsigned char)uri[i+1]) ||
                !isxdigit((unsigned char)uri[i+2])) return -1;
            /* Check for encoded null byte */
            if (uri[i+1] == '0' && uri[i+2] == '0') return -1;
        }
    }

    return 0;
}

/* ── Request body validation (65.2.2) ────────────────────────────────── */

static int g_max_json_depth = 32;
static int g_max_form_fields = 1000;

void security_set_body_limits(int max_json_depth, int max_form_fields)
{
    if (max_json_depth > 0) g_max_json_depth = max_json_depth;
    if (max_form_fields > 0) g_max_form_fields = max_form_fields;
}

/* Check JSON depth. Returns 0 if OK, -1 if too deep. */
int security_check_json_depth(const char *json)
{
    if (!json) return 0;
    int depth = 0, max_depth = 0;
    for (const char *p = json; *p; p++) {
        if (*p == '{' || *p == '[') {
            depth++;
            if (depth > max_depth) max_depth = depth;
            if (depth > g_max_json_depth) return -1;
        } else if (*p == '}' || *p == ']') {
            depth--;
        } else if (*p == '"') {
            /* Skip string contents */
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && *(p+1)) p++;
                p++;
            }
        }
    }
    return 0;
}

/* ── SQL injection detection (65.2.3) ────────────────────────────────── */

/* Common SQL injection patterns (OWASP-inspired) */
static const char *sqli_patterns[] = {
    "' OR '",  "' or '",  "1=1",  "1 = 1",
    "'; DROP", "'; drop", "UNION SELECT", "union select",
    "' --",    "';--",    "/**/",
    "EXEC(",   "exec(",   "xp_cmdshell",
    "CHAR(",   "char(",   "CONCAT(",
    "WAITFOR", "waitfor", "BENCHMARK(",
    "SLEEP(",  "sleep(",
    NULL
};

/* Check a string for SQL injection patterns.
 * Returns 0 if clean, -1 if suspicious. */
int security_check_sqli(const char *input)
{
    if (!input) return 0;
    for (int i = 0; sqli_patterns[i]; i++) {
        if (strstr(input, sqli_patterns[i])) return -1;
    }
    return 0;
}

/* ── XSS detection (65.2.4) ──────────────────────────────────────────── */

static const char *xss_patterns[] = {
    "<script",    "<SCRIPT",    "javascript:",  "JAVASCRIPT:",
    "onerror=",   "onload=",    "onclick=",     "onmouseover=",
    "onfocus=",   "onblur=",    "oninput=",
    "<iframe",    "<IFRAME",    "<object",      "<OBJECT",
    "<embed",     "<EMBED",     "<svg",         "<SVG",
    "expression(", "eval(",     "document.cookie",
    "window.location", "document.write",
    NULL
};

/* Check a string for XSS patterns.
 * Returns 0 if clean, -1 if suspicious. */
int security_check_xss(const char *input)
{
    if (!input) return 0;
    for (int i = 0; xss_patterns[i]; i++) {
        if (strcasestr(input, xss_patterns[i])) return -1;
    }
    return 0;
}

/* ── WAF rule engine (65.2.5) ────────────────────────────────────────── */

typedef enum {
    WAF_MATCH_URI,
    WAF_MATCH_HEADER,
    WAF_MATCH_BODY,
    WAF_MATCH_METHOD,
    WAF_MATCH_QUERY,
} WafMatchTarget;

typedef enum {
    WAF_ACTION_ALLOW,
    WAF_ACTION_DENY,
    WAF_ACTION_LOG,
    WAF_ACTION_REDIRECT,
} WafAction;

typedef struct {
    WafMatchTarget target;
    char          *pattern;    /* substring match */
    WafAction      action;
    uint32_t       deny_status; /* status code for DENY */
    char          *redirect_url;
    char          *description;
} WafRule;

#define MAX_WAF_RULES 128

static WafRule g_waf_rules[MAX_WAF_RULES];
static int     g_waf_rule_count = 0;

int security_add_waf_rule(WafMatchTarget target, const char *pattern,
                           WafAction action, uint32_t deny_status,
                           const char *description)
{
    if (g_waf_rule_count >= MAX_WAF_RULES) return -1;
    WafRule *r = &g_waf_rules[g_waf_rule_count++];
    r->target = target;
    r->pattern = strdup(pattern);
    r->action = action;
    r->deny_status = deny_status > 0 ? deny_status : 403;
    r->redirect_url = NULL;
    r->description = description ? strdup(description) : NULL;
    return 0;
}

/* Evaluate WAF rules against a request.
 * Returns WAF_ACTION_ALLOW if no rules matched.
 * Sets *deny_status if action is DENY. */
WafAction security_waf_check(const char *method, const char *uri,
                              const char *body, const char *const *hdr_names,
                              const char *const *hdr_values, int nhdr,
                              uint32_t *out_deny_status)
{
    for (int i = 0; i < g_waf_rule_count; i++) {
        WafRule *r = &g_waf_rules[i];
        const char *haystack = NULL;

        switch (r->target) {
        case WAF_MATCH_URI:    haystack = uri;    break;
        case WAF_MATCH_BODY:   haystack = body;   break;
        case WAF_MATCH_METHOD: haystack = method; break;
        case WAF_MATCH_QUERY:
            /* Extract query string from URI */
            if (uri) {
                const char *q = strchr(uri, '?');
                haystack = q ? q + 1 : NULL;
            }
            break;
        case WAF_MATCH_HEADER:
            /* Check all headers */
            for (int h = 0; h < nhdr; h++) {
                if (strstr(hdr_values[h], r->pattern)) {
                    if (r->action == WAF_ACTION_DENY && out_deny_status)
                        *out_deny_status = r->deny_status;
                    return r->action;
                }
            }
            continue;
        }

        if (haystack && strstr(haystack, r->pattern)) {
            if (r->action == WAF_ACTION_DENY && out_deny_status)
                *out_deny_status = r->deny_status;
            return r->action;
        }
    }

    return WAF_ACTION_ALLOW;
}

/* ── CSP header builder (65.3.3) ─────────────────────────────────────── */

typedef struct {
    char *default_src;
    char *script_src;
    char *style_src;
    char *img_src;
    char *connect_src;
    char *font_src;
    char *object_src;
    char *media_src;
    char *frame_src;
    char *base_uri;
    char *form_action;
    char *frame_ancestors;
    char *report_uri;
} CspPolicy;

static CspPolicy g_csp = {0};

void security_csp_set(const char *directive, const char *value)
{
    if (!directive || !value) return;

    char **target = NULL;
    if (strcasecmp(directive, "default-src") == 0) target = &g_csp.default_src;
    else if (strcasecmp(directive, "script-src") == 0) target = &g_csp.script_src;
    else if (strcasecmp(directive, "style-src") == 0) target = &g_csp.style_src;
    else if (strcasecmp(directive, "img-src") == 0) target = &g_csp.img_src;
    else if (strcasecmp(directive, "connect-src") == 0) target = &g_csp.connect_src;
    else if (strcasecmp(directive, "font-src") == 0) target = &g_csp.font_src;
    else if (strcasecmp(directive, "object-src") == 0) target = &g_csp.object_src;
    else if (strcasecmp(directive, "media-src") == 0) target = &g_csp.media_src;
    else if (strcasecmp(directive, "frame-src") == 0) target = &g_csp.frame_src;
    else if (strcasecmp(directive, "base-uri") == 0) target = &g_csp.base_uri;
    else if (strcasecmp(directive, "form-action") == 0) target = &g_csp.form_action;
    else if (strcasecmp(directive, "frame-ancestors") == 0) target = &g_csp.frame_ancestors;
    else if (strcasecmp(directive, "report-uri") == 0) target = &g_csp.report_uri;

    if (target) {
        free(*target);
        *target = strdup(value);
    }
}

/* Build the full CSP header value. Caller owns returned string. */
char *security_csp_build(void)
{
    size_t cap = 2048;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    int off = 0;

    #define CSP_APPEND(name, field) \
        if (g_csp.field) { \
            if (off > 0) off += snprintf(buf + off, cap - (size_t)off, "; "); \
            off += snprintf(buf + off, cap - (size_t)off, "%s %s", name, g_csp.field); \
        }

    CSP_APPEND("default-src", default_src)
    CSP_APPEND("script-src", script_src)
    CSP_APPEND("style-src", style_src)
    CSP_APPEND("img-src", img_src)
    CSP_APPEND("connect-src", connect_src)
    CSP_APPEND("font-src", font_src)
    CSP_APPEND("object-src", object_src)
    CSP_APPEND("media-src", media_src)
    CSP_APPEND("frame-src", frame_src)
    CSP_APPEND("base-uri", base_uri)
    CSP_APPEND("form-action", form_action)
    CSP_APPEND("frame-ancestors", frame_ancestors)
    CSP_APPEND("report-uri", report_uri)

    #undef CSP_APPEND

    if (off == 0) { free(buf); return NULL; }
    return buf;
}

/* ── Per-route CORS (65.3.4) ─────────────────────────────────────────── */

typedef struct {
    char *path_prefix;
    char *allowed_origins;
    char *allowed_methods;
    char *allowed_headers;
    int   allow_credentials;
    int   max_age;
} CorsPolicy;

#define MAX_CORS_POLICIES 32

static CorsPolicy g_cors_policies[MAX_CORS_POLICIES];
static int        g_cors_policy_count = 0;

void security_cors_add(const char *path_prefix, const char *origins,
                        const char *methods, const char *headers,
                        int credentials, int max_age)
{
    if (g_cors_policy_count >= MAX_CORS_POLICIES) return;
    CorsPolicy *p = &g_cors_policies[g_cors_policy_count++];
    p->path_prefix = strdup(path_prefix);
    p->allowed_origins = strdup(origins);
    p->allowed_methods = methods ? strdup(methods) : strdup("GET, POST, OPTIONS");
    p->allowed_headers = headers ? strdup(headers) : strdup("Content-Type");
    p->allow_credentials = credentials;
    p->max_age = max_age > 0 ? max_age : 86400;
}

/* Find the CORS policy for a given path. Returns NULL if none. */
CorsPolicy *security_cors_find(const char *path)
{
    if (!path) return NULL;
    CorsPolicy *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < g_cors_policy_count; i++) {
        size_t plen = strlen(g_cors_policies[i].path_prefix);
        if (strncmp(path, g_cors_policies[i].path_prefix, plen) == 0) {
            if (plen > best_len) {
                best = &g_cors_policies[i];
                best_len = plen;
            }
        }
    }
    return best;
}

/* ── Sub-request authorization (65.3.1) ──────────────────────────────── */

static char *g_auth_endpoint = NULL;

void security_set_auth_endpoint(const char *url)
{
    free(g_auth_endpoint);
    g_auth_endpoint = url ? strdup(url) : NULL;
}

const char *security_get_auth_endpoint(void)
{
    return g_auth_endpoint;
}

/* ── Client certificate configuration (65.3.2) ──────────────────────── */

static char *g_client_ca_path = NULL;
static int   g_client_cert_required = 0;

void security_set_client_cert(const char *ca_path, int required)
{
    free(g_client_ca_path);
    g_client_ca_path = ca_path ? strdup(ca_path) : NULL;
    g_client_cert_required = required;
}

const char *security_get_client_ca(void) { return g_client_ca_path; }
int security_client_cert_required(void) { return g_client_cert_required; }
