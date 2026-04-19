/*
 * hooks.c — Scripting and extensibility for std.http.
 *
 * Epic 69: Request pipeline hooks (post-accept, pre-route, post-route,
 * log), TOML-based server configuration, runtime configuration API,
 * modular feature loading.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>

/* ── Hook types (69.1.1 – 69.1.4) ──────────────────────────────────── */

/*
 * Hook phases in the request pipeline:
 *   1. post-accept  — after accept(), before read (connection-level)
 *   2. pre-route    — after parsing, before dispatch (can rewrite)
 *   3. post-route   — after handler, before send (can modify response)
 *   4. log          — after send (for analytics, non-blocking)
 */

/* Request metadata passed to hooks */
typedef struct {
    const char *client_ip;
    int         client_fd;
    const char *method;
    const char *uri;
    const char *body;
    uint64_t    body_len;
    /* Headers as parallel key/value arrays */
    const char **header_keys;
    const char **header_vals;
    uint64_t     header_count;
} HookRequest;

/* Response metadata passed to post-route and log hooks */
typedef struct {
    uint16_t    status;
    const char *body;
    uint64_t    body_len;
    const char **header_keys;
    const char **header_vals;
    uint64_t     header_count;
    double       latency_ms;
} HookResponse;

/*
 * Hook action — return value from pre-route and post-accept hooks.
 * HOOK_CONTINUE: pass through to next hook / route handler.
 * HOOK_SHORT_CIRCUIT: respond immediately with the provided response.
 * HOOK_REJECT: reject connection (post-accept only).
 */
typedef enum {
    HOOK_CONTINUE,
    HOOK_SHORT_CIRCUIT,
    HOOK_REJECT,
} HookAction;

/* Hook result — returned by hooks that can short-circuit */
typedef struct {
    HookAction  action;
    uint16_t    status;       /* for SHORT_CIRCUIT */
    const char *body;         /* for SHORT_CIRCUIT */
    const char *redirect_uri; /* for SHORT_CIRCUIT redirect */
    /* URI rewrite (pre-route only) */
    const char *rewrite_uri;
    /* Header modifications */
    const char *add_header_key;
    const char *add_header_val;
    const char *remove_header_key;
} HookResult;

/* ── Hook function signatures ───────────────────────────────────────── */

/* Post-accept: can reject connections based on IP, metadata */
typedef HookResult (*PostAcceptHook)(const char *client_ip, int client_fd);

/* Pre-route: can rewrite URI, modify headers, short-circuit */
typedef HookResult (*PreRouteHook)(HookRequest *req);

/* Post-route: can modify response before sending */
typedef HookResult (*PostRouteHook)(HookRequest *req, HookResponse *resp);

/* Log: non-blocking post-send callback for analytics */
typedef void (*LogHook)(const HookRequest *req, const HookResponse *resp);

/* ── Hook registry ──────────────────────────────────────────────────── */

#define MAX_HOOKS 16

static PostAcceptHook g_post_accept_hooks[MAX_HOOKS];
static int            g_post_accept_count = 0;

static PreRouteHook   g_pre_route_hooks[MAX_HOOKS];
static int            g_pre_route_count = 0;

static PostRouteHook  g_post_route_hooks[MAX_HOOKS];
static int            g_post_route_count = 0;

static LogHook        g_log_hooks[MAX_HOOKS];
static int            g_log_count = 0;

/* ── Registration functions (69.1.1 – 69.1.4) ─────────────────────── */

int hook_register_post_accept(PostAcceptHook fn)
{
    if (g_post_accept_count >= MAX_HOOKS || !fn) return -1;
    g_post_accept_hooks[g_post_accept_count++] = fn;
    return 0;
}

int hook_register_pre_route(PreRouteHook fn)
{
    if (g_pre_route_count >= MAX_HOOKS || !fn) return -1;
    g_pre_route_hooks[g_pre_route_count++] = fn;
    return 0;
}

int hook_register_post_route(PostRouteHook fn)
{
    if (g_post_route_count >= MAX_HOOKS || !fn) return -1;
    g_post_route_hooks[g_post_route_count++] = fn;
    return 0;
}

int hook_register_log(LogHook fn)
{
    if (g_log_count >= MAX_HOOKS || !fn) return -1;
    g_log_hooks[g_log_count++] = fn;
    return 0;
}

/* ── Hook execution ─────────────────────────────────────────────────── */

/*
 * hook_run_post_accept — run all post-accept hooks.
 * Returns HOOK_REJECT if any hook rejects the connection.
 */
HookAction hook_run_post_accept(const char *client_ip, int client_fd)
{
    for (int i = 0; i < g_post_accept_count; i++) {
        HookResult r = g_post_accept_hooks[i](client_ip, client_fd);
        if (r.action == HOOK_REJECT)
            return HOOK_REJECT;
    }
    return HOOK_CONTINUE;
}

/*
 * hook_run_pre_route — run all pre-route hooks.
 * May modify req->uri (rewrite). Returns SHORT_CIRCUIT result if
 * any hook short-circuits.
 */
HookResult hook_run_pre_route(HookRequest *req)
{
    HookResult last;
    memset(&last, 0, sizeof(last));
    last.action = HOOK_CONTINUE;

    for (int i = 0; i < g_pre_route_count; i++) {
        HookResult r = g_pre_route_hooks[i](req);
        if (r.rewrite_uri)
            req->uri = r.rewrite_uri;
        if (r.action == HOOK_SHORT_CIRCUIT)
            return r;
    }
    return last;
}

/*
 * hook_run_post_route — run all post-route hooks.
 * May modify response headers, body, or status.
 */
HookResult hook_run_post_route(HookRequest *req, HookResponse *resp)
{
    HookResult last;
    memset(&last, 0, sizeof(last));
    last.action = HOOK_CONTINUE;

    for (int i = 0; i < g_post_route_count; i++) {
        HookResult r = g_post_route_hooks[i](req, resp);
        if (r.action == HOOK_SHORT_CIRCUIT)
            return r;
    }
    return last;
}

/*
 * hook_run_log — run all log hooks (non-blocking, fire-and-forget).
 */
void hook_run_log(const HookRequest *req, const HookResponse *resp)
{
    for (int i = 0; i < g_log_count; i++)
        g_log_hooks[i](req, resp);
}

/* ── Hook cleanup ───────────────────────────────────────────────────── */

void hook_clear_all(void)
{
    g_post_accept_count = 0;
    g_pre_route_count   = 0;
    g_post_route_count  = 0;
    g_log_count         = 0;
}

/* ── TOML-based server configuration (69.2.1) ──────────────────────── */

/* Forward declaration for toml parser (from toml.c / tomlc99) */
typedef struct toml_table_t toml_table_t;
extern toml_table_t *toml_parse_file(FILE *fp, char *errbuf, int errbufsz);
extern void          toml_free(toml_table_t *tab);
extern toml_table_t *toml_table_in(const toml_table_t *tab, const char *key);
extern int           toml_rtos(const char *s, char **ret);
extern int           toml_rtoi(const char *s, int64_t *ret);
extern int           toml_rtob(const char *s, int *ret);
extern const char   *toml_raw_in(const toml_table_t *tab, const char *key);

typedef struct {
    char    *host;
    uint64_t port;
    uint64_t workers;
    uint64_t timeout_secs;
    uint64_t max_header_size;
    uint64_t max_body_size;
    char    *cert_path;
    char    *key_path;
    char    *access_log;
    char    *error_log;
    int      compression;     /* 1 = enable gzip/br */
    int      caching;         /* 1 = enable cache layer */
    int      rate_limit_rps;  /* 0 = disabled */
    int      waf;             /* 1 = enable WAF */
} ServerConfig;

/*
 * config_load_toml — load server configuration from a TOML file.
 * Returns heap-allocated ServerConfig. Caller owns.
 */
ServerConfig *config_load_toml(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "config: cannot open %s\n", path);
        return NULL;
    }

    char errbuf[256];
    toml_table_t *root = toml_parse_file(fp, errbuf, sizeof(errbuf));
    fclose(fp);
    if (!root) {
        fprintf(stderr, "config: parse error: %s\n", errbuf);
        return NULL;
    }

    ServerConfig *cfg = calloc(1, sizeof(ServerConfig));
    if (!cfg) { toml_free(root); return NULL; }

    /* Defaults */
    cfg->port = 8080;
    cfg->workers = 4;
    cfg->timeout_secs = 30;
    cfg->max_header_size = 8192;
    cfg->max_body_size = 1048576;
    cfg->compression = 1;

    /* [server] section */
    toml_table_t *server = toml_table_in(root, "server");
    if (server) {
        const char *raw;
        int64_t iv;
        char *sv;

        raw = toml_raw_in(server, "host");
        if (raw && toml_rtos(raw, &sv) == 0) cfg->host = sv;

        raw = toml_raw_in(server, "port");
        if (raw && toml_rtoi(raw, &iv) == 0) cfg->port = (uint64_t)iv;

        raw = toml_raw_in(server, "workers");
        if (raw && toml_rtoi(raw, &iv) == 0) cfg->workers = (uint64_t)iv;

        raw = toml_raw_in(server, "timeout");
        if (raw && toml_rtoi(raw, &iv) == 0) cfg->timeout_secs = (uint64_t)iv;

        raw = toml_raw_in(server, "max_header_size");
        if (raw && toml_rtoi(raw, &iv) == 0) cfg->max_header_size = (uint64_t)iv;

        raw = toml_raw_in(server, "max_body_size");
        if (raw && toml_rtoi(raw, &iv) == 0) cfg->max_body_size = (uint64_t)iv;
    }

    /* [tls] section */
    toml_table_t *tls = toml_table_in(root, "tls");
    if (tls) {
        const char *raw;
        char *sv;

        raw = toml_raw_in(tls, "cert");
        if (raw && toml_rtos(raw, &sv) == 0) cfg->cert_path = sv;

        raw = toml_raw_in(tls, "key");
        if (raw && toml_rtos(raw, &sv) == 0) cfg->key_path = sv;
    }

    /* [logging] section */
    toml_table_t *logging = toml_table_in(root, "logging");
    if (logging) {
        const char *raw;
        char *sv;

        raw = toml_raw_in(logging, "access_log");
        if (raw && toml_rtos(raw, &sv) == 0) cfg->access_log = sv;

        raw = toml_raw_in(logging, "error_log");
        if (raw && toml_rtos(raw, &sv) == 0) cfg->error_log = sv;
    }

    /* [features] section */
    toml_table_t *features = toml_table_in(root, "features");
    if (features) {
        const char *raw;
        int bv;
        int64_t iv;

        raw = toml_raw_in(features, "compression");
        if (raw && toml_rtob(raw, &bv) == 0) cfg->compression = bv;

        raw = toml_raw_in(features, "caching");
        if (raw && toml_rtob(raw, &bv) == 0) cfg->caching = bv;

        raw = toml_raw_in(features, "waf");
        if (raw && toml_rtob(raw, &bv) == 0) cfg->waf = bv;

        raw = toml_raw_in(features, "rate_limit_rps");
        if (raw && toml_rtoi(raw, &iv) == 0) cfg->rate_limit_rps = (int)iv;
    }

    toml_free(root);
    return cfg;
}

void config_free(ServerConfig *cfg)
{
    if (!cfg) return;
    free(cfg->host);
    free(cfg->cert_path);
    free(cfg->key_path);
    free(cfg->access_log);
    free(cfg->error_log);
    free(cfg);
}

/* ── Runtime configuration API (69.2.2) ─────────────────────────────── */

/*
 * Runtime config: admin endpoint that allows querying and updating
 * configuration without restart. Protected by auth token.
 */

static char *g_admin_token = NULL;

void config_set_admin_token(const char *token)
{
    free(g_admin_token);
    g_admin_token = token ? strdup(token) : NULL;
}

/*
 * config_admin_auth — check authorization for admin endpoints.
 * Returns 1 if authorized, 0 if not.
 */
int config_admin_auth(const char *auth_header)
{
    if (!g_admin_token) return 0; /* no token = disabled */
    if (!auth_header) return 0;

    /* Expect: Bearer <token> */
    if (strncmp(auth_header, "Bearer ", 7) != 0) return 0;
    return strcmp(auth_header + 7, g_admin_token) == 0;
}

/*
 * config_runtime_get — return current config as JSON.
 * Caller owns returned string.
 */
char *config_runtime_get(const ServerConfig *cfg)
{
    if (!cfg) return NULL;
    char *buf = malloc(2048);
    if (!buf) return NULL;

    snprintf(buf, 2048,
        "{\"host\":\"%s\",\"port\":%llu,\"workers\":%llu,"
        "\"timeout\":%llu,\"max_header_size\":%llu,"
        "\"max_body_size\":%llu,\"compression\":%s,"
        "\"caching\":%s,\"waf\":%s,\"rate_limit_rps\":%d}",
        cfg->host ? cfg->host : "0.0.0.0",
        (unsigned long long)cfg->port,
        (unsigned long long)cfg->workers,
        (unsigned long long)cfg->timeout_secs,
        (unsigned long long)cfg->max_header_size,
        (unsigned long long)cfg->max_body_size,
        cfg->compression ? "true" : "false",
        cfg->caching ? "true" : "false",
        cfg->waf ? "true" : "false",
        cfg->rate_limit_rps);
    return buf;
}

/*
 * config_runtime_update — apply a JSON key=value update to config.
 * Supports: timeout, rate_limit_rps, compression, caching, waf.
 * Returns 0 on success, -1 on error.
 */
int config_runtime_update(ServerConfig *cfg, const char *key, const char *value)
{
    if (!cfg || !key || !value) return -1;

    if (strcmp(key, "timeout") == 0) {
        cfg->timeout_secs = (uint64_t)atoi(value);
    } else if (strcmp(key, "rate_limit_rps") == 0) {
        cfg->rate_limit_rps = atoi(value);
    } else if (strcmp(key, "compression") == 0) {
        cfg->compression = (strcmp(value, "true") == 0);
    } else if (strcmp(key, "caching") == 0) {
        cfg->caching = (strcmp(value, "true") == 0);
    } else if (strcmp(key, "waf") == 0) {
        cfg->waf = (strcmp(value, "true") == 0);
    } else {
        return -1;
    }
    return 0;
}

/* ── Modular feature loading (69.2.3) ───────────────────────────────── */

/*
 * Feature flags — compile-time inclusion/exclusion of capabilities.
 * Each feature is guarded by a TK_HAVE_* define.
 * This module provides runtime query of available features.
 */

typedef struct {
    const char *name;
    int         available;
} FeatureInfo;

/*
 * config_list_features — return array of available features.
 * out_count is set to the number of features.
 * Returns static array (do not free).
 */
const FeatureInfo *config_list_features(int *out_count)
{
    static FeatureInfo features[] = {
        { "http2",
#ifdef TK_HAVE_OPENSSL
          1
#else
          0
#endif
        },
        { "tls",
#ifdef TK_HAVE_OPENSSL
          1
#else
          0
#endif
        },
        { "acme",
#ifdef TK_HAVE_OPENSSL
          1
#else
          0
#endif
        },
        { "brotli",
#ifdef TK_HAVE_BROTLI
          1
#else
          0
#endif
        },
        { "zstd",
#ifdef TK_HAVE_ZSTD
          1
#else
          0
#endif
        },
        { "waf", 1 },
        { "cache", 1 },
        { "proxy", 1 },
        { "metrics", 1 },
        { "websocket", 1 },
        { "sse", 1 },
    };

    if (out_count) *out_count = (int)(sizeof(features) / sizeof(features[0]));
    return features;
}

/*
 * config_has_feature — check if a named feature is available.
 */
int config_has_feature(const char *name)
{
    int count = 0;
    const FeatureInfo *features = config_list_features(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(features[i].name, name) == 0)
            return features[i].available;
    }
    return 0;
}
