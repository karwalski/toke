/*
 * tk_web_glue.c — i64-ABI wrappers for stdlib modules used in compiled toke programs.
 *
 * The toke compiler emits all function arguments as i64 (pointer-as-integer)
 * for external stdlib calls.  These wrappers bridge the i64 ABI to the proper
 * C pointer types required by str.c, http.c, and env.c.
 *
 * Linked by compile_binary() alongside the user program's LLVM IR.
 *
 * Story: 46.1.1 (stdlib linking for compiled programs)
 * Story: 56.8.7 (ooke toke-only build — file/path/toml/args/log wrappers)
 */

#include "str.h"
#include "http.h"
#include "env.h"
#include "log.h"
#include "router.h"
#include "file.h"
#include "path.h"
#include "args.h"
#include "toml.h"
#include "md.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string.h>

/* ── str wrappers ─────────────────────────────────────────────────────── */

int64_t tk_str_concat_w(int64_t a, int64_t b) {
    return (int64_t)(intptr_t)str_concat(
        (const char *)(intptr_t)a,
        (const char *)(intptr_t)b);
}

int64_t tk_str_len_w(int64_t s) {
    return (int64_t)str_len((const char *)(intptr_t)s);
}

int64_t tk_str_trim_w(int64_t s) {
    return (int64_t)(intptr_t)str_trim((const char *)(intptr_t)s);
}

int64_t tk_str_upper_w(int64_t s) {
    return (int64_t)(intptr_t)str_upper((const char *)(intptr_t)s);
}

int64_t tk_str_lower_w(int64_t s) {
    return (int64_t)(intptr_t)str_lower((const char *)(intptr_t)s);
}

int64_t tk_str_from_int(int64_t n) {
    return (int64_t)(intptr_t)str_from_int(n);
}

int64_t tk_str_to_int(int64_t s) {
    if (!s) return 0;
    return (int64_t)atoll((const char *)(intptr_t)s);
}

int64_t tk_str_from_float(int64_t n_as_double_bits) {
    double d;
    memcpy(&d, &n_as_double_bits, sizeof d);
    return (int64_t)(intptr_t)str_from_float(d);
}

/*
 * tk_str_split_w — split a toke string and return a toke-format array.
 *
 * Allocates a block of (len+1) i64 slots:
 *   block[0]     = element count (length)
 *   block[1..N]  = each element as i64 (const char * as integer)
 * Returns (block+1) so that ptr[-1] == length and ptr[i] == element i,
 * matching the toke runtime array layout.
 */
int64_t tk_str_split_w(int64_t s, int64_t sep) {
    if (!s || !sep) return 0;
    StrArray arr = str_split((const char *)(intptr_t)s, (const char *)(intptr_t)sep);
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    return (int64_t)(intptr_t)(block + 1);
}

/* ── additional str wrappers (Story 56.8.7) ──────────────────────────── */

int64_t tk_str_indexof_w(int64_t s, int64_t sub) {
    if (!s || !sub) return -1;
    return str_index((const char *)(intptr_t)s, (const char *)(intptr_t)sub);
}

int64_t tk_str_slice_w(int64_t s, int64_t start, int64_t end_) {
    if (!s) return 0;
    StrSliceResult r = str_slice((const char *)(intptr_t)s, (uint64_t)start, (uint64_t)end_);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_str_replace_w(int64_t s, int64_t old_val, int64_t new_val) {
    if (!s || !old_val) return s;
    return (int64_t)(intptr_t)str_replace(
        (const char *)(intptr_t)s,
        (const char *)(intptr_t)old_val,
        (const char *)(intptr_t)new_val);
}

int64_t tk_str_startswith_w(int64_t s, int64_t prefix) {
    if (!s || !prefix) return 0;
    return (int64_t)str_starts_with((const char *)(intptr_t)s, (const char *)(intptr_t)prefix);
}

int64_t tk_str_endswith_w(int64_t s, int64_t suffix) {
    if (!s || !suffix) return 0;
    return (int64_t)str_ends_with((const char *)(intptr_t)s, (const char *)(intptr_t)suffix);
}

int64_t tk_str_trimprefix_w(int64_t s, int64_t prefix) {
    if (!s || !prefix) return s;
    return (int64_t)(intptr_t)str_trimprefix((const char *)(intptr_t)s, (const char *)(intptr_t)prefix);
}

int64_t tk_str_trimsuffix_w(int64_t s, int64_t suffix) {
    if (!s || !suffix) return s;
    return (int64_t)(intptr_t)str_trimsuffix((const char *)(intptr_t)s, (const char *)(intptr_t)suffix);
}

int64_t tk_str_lastindex_w(int64_t s, int64_t sub) {
    if (!s || !sub) return -1;
    return str_lastindex((const char *)(intptr_t)s, (const char *)(intptr_t)sub);
}

/*
 * tk_str_matchbracket_w — match a [...] bracket at the start of s.
 * Returns the inner content as i64 (ptr to string), or 0 on error.
 */
int64_t tk_str_matchbracket_w(int64_t s) {
    if (!s) return 0;
    StrBracketResult r = str_matchbracket((const char *)(intptr_t)s);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

/* ── log wrappers ─────────────────────────────────────────────────────── */

/*
 * tk_log_open_access_w — open a rotating HTTP access log and register it as
 * the global access log used by http.c.
 *
 * path_ptr:     i64 (const char *) — path to the log file, e.g. "logs/access.log"
 * max_lines:    i64 — rotate after this many lines (0 = no rotation)
 * max_files:    i64 — keep at most N rotated .gz files (0 = no limit)
 * max_age_days: i64 — delete files older than N days (0 = disabled; >0 wins)
 * Returns: 0 on success, -1 if the log file could not be opened.
 */
int64_t tk_log_open_access_w(int64_t path_ptr, int64_t max_lines,
                               int64_t max_files, int64_t max_age_days)
{
    const char *path = (const char *)(intptr_t)path_ptr;
    TkAccessLog *al = tk_access_log_open(path, (int)max_lines,
                                          (int)max_files, (int)max_age_days);
    if (!al) return -1;
    tk_access_log_set_global(al);
    return 0;
}

/* ── env wrappers ─────────────────────────────────────────────────────── */

int64_t tk_env_get_or(int64_t key, int64_t def) {
    return (int64_t)(intptr_t)env_get_or(
        (const char *)(intptr_t)key,
        (const char *)(intptr_t)def);
}

/* ── http wrappers ────────────────────────────────────────────────────── */

/*
 * Static route registry.
 *
 * Stores up to TK_MAX_STATIC_ROUTES path→body mappings registered via
 * http.getstatic.  A single dispatch handler performs a linear path lookup
 * at request time, avoiding the need for per-route closure functions.
 */
#define TK_MAX_STATIC_ROUTES 512

typedef struct {
    const char *path;
    const char *body;
    int         status;
} TkStaticRoute;

static TkStaticRoute g_static_routes[TK_MAX_STATIC_ROUTES];
static int           g_static_route_count = 0;

static Res tk_static_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_static_route_count; i++) {
        if (g_static_routes[i].path &&
            strcmp(g_static_routes[i].path, rpath) == 0) {
            Res r;
            r.status       = g_static_routes[i].status;
            r.body         = g_static_routes[i].body;
            r.headers.data = NULL;
            r.headers.len  = 0;
            return r;
        }
    }
    Res r;
    r.status       = 404;
    r.body         = "Not Found";
    r.headers.data = NULL;
    r.headers.len  = 0;
    return r;
}

/*
 * tk_http_get_static — Register a static GET handler returning a fixed body.
 *
 * path_ptr: i64 (const char * as integer) — URL path pattern e.g. "/"
 * body_ptr: i64 (const char * as integer) — response body string
 * Returns:  0 (i64)
 */
int64_t tk_http_get_static(int64_t path_ptr, int64_t body_ptr) {
    if (g_static_route_count >= TK_MAX_STATIC_ROUTES) return -1;
    int i = g_static_route_count++;
    g_static_routes[i].path   = (const char *)(intptr_t)path_ptr;
    g_static_routes[i].body   = (const char *)(intptr_t)body_ptr;
    g_static_routes[i].status = 200;
    http_GET((const char *)(intptr_t)path_ptr, tk_static_dispatch);
    return 0;
}

/*
 * tk_http_serve — Start the HTTP server on the given port (blocks until SIGINT/SIGTERM).
 *
 * port: i64 — TCP port number (1-65535; defaults to 8080 if out of range)
 * Returns: 0 on clean shutdown, -1 on bind/listen failure.
 */
int64_t tk_http_serve(int64_t port) {
    int p = (int)port;
    if (p <= 0 || p > 65535) p = 8080;
    return (int64_t)http_serve((uint16_t)p);
}

/*
 * tk_http_servetls — Start the HTTPS server on the given port.
 *
 * Captures the current global route table (registered via tk_http_get_static),
 * loads the TLS cert/key, then blocks serving HTTPS until SIGINT/SIGTERM.
 *
 * port:     i64 (const char * as integer) — port number as string, or i64 literal
 * cert_ptr: i64 (const char * as integer) — path to PEM cert file
 * key_ptr:  i64 (const char * as integer) — path to PEM key file
 * Returns:  0 on clean shutdown, -1 on error.
 */
int64_t tk_http_servetls(int64_t port, int64_t cert_ptr, int64_t key_ptr) {
    int p = (int)port;
    if (p <= 0 || p > 65535) p = 8443;
    const char *cert = (const char *)(intptr_t)cert_ptr;
    const char *key  = (const char *)(intptr_t)key_ptr;
    TkTlsCtx *tls = http_tls_ctx_new(cert, key);
    if (!tls) return -1;
    TkHttpRouter *router = http_router_new();
    if (!router) { http_tls_ctx_free(tls); return -1; }
    TkHttpErr err = http_serve_tls(router, NULL, (uint64_t)p, tls);
    http_router_free(router);
    http_tls_ctx_free(tls);
    return (err == TK_HTTP_OK) ? 0 : -1;
}

/* ── Static file directory handler ───────────────────────────────────── */

/*
 * tk_http_serve_staticdir_w — Register a fallback handler that serves files
 * from a local directory for a given URL prefix.
 *
 * e.g. tk_http_serve_staticdir_w("/static", "/path/to/project") will serve
 * GET /static/css/style.css from /path/to/project/static/css/style.css.
 *
 * Registered as a "*" wildcard route (last-resort fallback).
 */

static char g_staticdir_prefix[512];
static char g_staticdir_root[1024];

static const char *mime_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif"))  return "image/gif";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".woff")) return "font/woff";
    if (!strcmp(dot, ".woff2")) return "font/woff2";
    if (!strcmp(dot, ".ttf"))  return "font/ttf";
    if (!strcmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".xml"))  return "application/xml";
    return "application/octet-stream";
}

static Res mk404(void) {
    Res r; r.status = 404; r.body = "Not Found";
    r.headers.data = NULL; r.headers.len = 0; return r;
}

static Res staticdir_handler(Req req) {
    if (!req.path) return mk404();
    size_t pfxlen = strlen(g_staticdir_prefix);
    if (strncmp(req.path, g_staticdir_prefix, pfxlen) != 0)
        return mk404();
    /* Build local file path: root + req.path */
    char filepath[2048];
    snprintf(filepath, sizeof filepath, "%s%s", g_staticdir_root, req.path);
    /* Reject path traversal */
    if (strstr(filepath, "..")) {
        Res r; r.status = 403; r.body = "Forbidden";
        r.headers.data = NULL; r.headers.len = 0; return r;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) return mk404();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        Res r; r.status = 500; r.body = "File too large";
        r.headers.data = NULL; r.headers.len = 0; return r;
    }
    char *body = malloc((size_t)sz + 1);
    if (!body) {
        fclose(f);
        Res r; r.status = 500; r.body = "OOM";
        r.headers.data = NULL; r.headers.len = 0; return r;
    }
    fread(body, 1, (size_t)sz, f);
    body[sz] = '\0';
    fclose(f);
    /* Build response with Content-Type header */
    const char *mime = mime_for_ext(filepath);
    Res res;
    res.status = 200;
    res.body   = body;
    StrPair *hdrs = malloc(sizeof(StrPair));
    if (hdrs) {
        hdrs[0].key = "Content-Type";
        hdrs[0].val = mime;
        res.headers.data = hdrs;
        res.headers.len  = 1;
    } else {
        res.headers.data = NULL;
        res.headers.len  = 0;
    }
    return res;
}

int64_t tk_http_serve_staticdir_w(int64_t prefix_i64, int64_t root_i64) {
    const char *prefix = (const char *)(intptr_t)prefix_i64;
    const char *root   = (const char *)(intptr_t)root_i64;
    if (!prefix || !root) return -1;
    snprintf(g_staticdir_prefix, sizeof g_staticdir_prefix, "%s", prefix);
    snprintf(g_staticdir_root,   sizeof g_staticdir_root,   "%s", root);
    http_GET("*", staticdir_handler);
    return 0;
}

/* ── vhost registry ───────────────────────────────────────────────────── */

/*
 * Virtual host registry: maps hostname → docroot.
 * Supports up to TK_MAX_VHOSTS entries.  The first registered entry is the
 * default vhost (used when no Host header matches).
 *
 * Story: vhost routing / static file serving
 */
#define TK_MAX_VHOSTS 8

typedef struct {
    const char *hostname;
    const char *docroot;
} TkVhost;

static TkVhost g_vhosts[TK_MAX_VHOSTS];
static int     g_vhost_count = 0;

/*
 * tk_http_vhost — Register a virtual host mapping hostname to docroot.
 *
 * hostname: i64 (const char *) — e.g. "example.com"
 * docroot:  i64 (const char *) — e.g. "/var/www/example"
 * Returns:  0 (i64)
 */
int64_t tk_http_vhost(int64_t hostname, int64_t docroot) {
    if (g_vhost_count >= TK_MAX_VHOSTS) return -1;
    int i = g_vhost_count++;
    g_vhosts[i].hostname = (const char *)(intptr_t)hostname;
    g_vhosts[i].docroot  = (const char *)(intptr_t)docroot;
    return 0;
}

/*
 * vhost_catchall_handler — catch-all Req handler for virtual host dispatch.
 *
 * Reads the Host: header, strips any trailing :port, looks up a matching
 * vhost docroot, then delegates to router_static_serve.
 * Falls back to the first registered vhost when no match is found.
 */
static Res vhost_catchall_handler(Req req) {
    /* Determine docroot from Host header */
    const char *docroot = NULL;
    HttpResult host_r = http_header(req, "Host");
    if (!host_r.is_err && host_r.ok) {
        /* Strip :port suffix — find last ':' */
        const char *host = host_r.ok;
        const char *colon = strrchr(host, ':');
        char hostname_buf[256];
        if (colon) {
            size_t len = (size_t)(colon - host);
            if (len >= sizeof hostname_buf) len = sizeof hostname_buf - 1;
            memcpy(hostname_buf, host, len);
            hostname_buf[len] = '\0';
            host = hostname_buf;
        }
        for (int i = 0; i < g_vhost_count; i++) {
            if (strcmp(g_vhosts[i].hostname, host) == 0) {
                docroot = g_vhosts[i].docroot;
                break;
            }
        }
    }
    /* Fallback: use first registered vhost */
    if (!docroot && g_vhost_count > 0)
        docroot = g_vhosts[0].docroot;

    if (!docroot) {
        Res r; r.status = 503; r.body = "No vhost configured";
        r.headers.data = NULL; r.headers.len = 0;
        return r;
    }

    /* Determine rel_path: strip leading '/' for router_static_serve */
    const char *url_path = req.path ? req.path : "/";
    const char *rel_path = (url_path[0] == '/') ? url_path + 1 : url_path;

    /* Get If-None-Match header for conditional GET support */
    HttpResult inm_r = http_header(req, "If-None-Match");
    const char *if_none_match = (!inm_r.is_err && inm_r.ok) ? inm_r.ok : NULL;

    TkRouteResp rr = router_static_serve(docroot, rel_path, if_none_match);

    /* Convert TkRouteResp → Res.
     * Always prepend a Content-Type header derived from rr.content_type,
     * then append any additional headers from the static-file response. */
    Res res;
    res.status = (uint16_t)rr.status;
    res.body   = rr.body;

    const char *ct = rr.content_type ? rr.content_type : "text/plain";
    uint64_t extra = rr.nheaders > 0 && rr.header_names && rr.header_values
                     ? rr.nheaders : 0;
    uint64_t total = 1 + extra; /* +1 for Content-Type */
    StrPair *pairs = malloc(total * sizeof(StrPair));
    if (pairs) {
        pairs[0].key = "Content-Type";
        pairs[0].val = ct;
        for (uint64_t i = 0; i < extra; i++) {
            pairs[1 + i].key = rr.header_names[i];
            pairs[1 + i].val = rr.header_values[i];
        }
        res.headers.data = pairs;
        res.headers.len  = total;
    } else {
        res.headers.data = NULL;
        res.headers.len  = 0;
    }
    return res;
}

/*
 * tk_http_servevhosts — Register the catch-all "*" route and start HTTP server.
 *
 * port: i64 — TCP port number
 * Returns: 0 on clean shutdown, -1 on failure.
 */
int64_t tk_http_servevhosts(int64_t port) {
    http_GET("*", vhost_catchall_handler);
    return tk_http_serve(port);
}

/*
 * tk_http_servevhoststls — Register the catch-all "*" route and start HTTPS server.
 *
 * port:     i64 — TCP port number
 * cert_ptr: i64 (const char *) — path to PEM cert file
 * key_ptr:  i64 (const char *) — path to PEM key file
 * Returns:  0 on clean shutdown, -1 on failure.
 */
int64_t tk_http_servevhoststls(int64_t port, int64_t cert_ptr, int64_t key_ptr) {
    http_GET("*", vhost_catchall_handler);
    return tk_http_servetls(port, cert_ptr, key_ptr);
}

/* ── http.serveworkers wrapper (Story 56.8.7) ────────────────────────── */

int64_t tk_http_serveworkers_w(int64_t port, int64_t workers) {
    int p = (int)port;
    if (p <= 0 || p > 65535) p = 8080;
    TkHttpRouter *r = http_router_new();
    if (!r) return -1;
    TkHttpErr err = http_serve_workers(r, NULL, (uint64_t)p, (int)workers);
    http_router_free(r);
    return (err == TK_HTTP_OK) ? 0 : -1;
}

/* ── file wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_file_read_w(int64_t path) {
    if (!path) return 0;
    StrFileResult r = file_read((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_file_write_w(int64_t path, int64_t content) {
    if (!path || !content) return 0;
    BoolFileResult r = file_write((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_isdir_w(int64_t path) {
    if (!path) return 0;
    return (int64_t)file_is_dir((const char *)(intptr_t)path);
}

int64_t tk_file_mkdir_w(int64_t path) {
    if (!path) return 0;
    BoolFileResult r = file_mkdir_p((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_copy_w(int64_t src, int64_t dst) {
    if (!src || !dst) return 0;
    BoolFileResult r = file_copy((const char *)(intptr_t)src, (const char *)(intptr_t)dst);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/*
 * tk_file_listall_w — list all files recursively and return a toke-format array.
 *
 * Returns (block+1) where block[0]=length, block[1..N]=file paths as i64.
 * Returns 0 on error.
 */
int64_t tk_file_listall_w(int64_t dir) {
    if (!dir) return 0;
    StrArrayFileResult r = file_listall((const char *)(intptr_t)dir);
    if (r.is_err) return 0;
    StrArray arr = r.ok;
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    return (int64_t)(intptr_t)(block + 1);
}

/* ── path wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_path_join_w(int64_t a, int64_t b) {
    if (!a || !b) return a ? a : b;
    return (int64_t)(intptr_t)path_join((const char *)(intptr_t)a, (const char *)(intptr_t)b);
}

int64_t tk_path_dir_w(int64_t p) {
    if (!p) return 0;
    return (int64_t)(intptr_t)path_dir((const char *)(intptr_t)p);
}

/* ── toml wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_toml_load_w(int64_t src) {
    if (!src) return 0;
    TomlResult r = toml_load((const char *)(intptr_t)src);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_section_w(int64_t tab, int64_t key) {
    if (!tab || !key) return tab;
    TomlResult r = toml_get_section((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? tab : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_str_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlStrResult r = toml_get_str((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_i64_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlI64Result r = toml_get_i64((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : r.ok;
}

int64_t tk_toml_bool_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlBoolResult r = toml_get_bool((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/* ── args wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_args_count_w(void) {
    return (int64_t)args_count();
}

int64_t tk_args_get_w(int64_t n) {
    StrArgsResult r = args_get((uint64_t)n);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

/* ── additional log wrappers (Story 56.8.7) ──────────────────────────── */

int64_t tk_log_info_w(int64_t msg, int64_t fields_map) {
    (void)fields_map;
    tk_log_info((const char *)(intptr_t)msg, NULL, 0);
    return 0;
}

int64_t tk_log_error_w(int64_t msg, int64_t fields_map) {
    (void)fields_map;
    tk_log_error((const char *)(intptr_t)msg, NULL, 0);
    return 0;
}

/* ── router.new wrapper (Story 56.8.7) ───────────────────────────────── */

int64_t tk_router_new_w(void) {
    TkRouter *r = router_new();
    return (int64_t)(intptr_t)r;
}

/* ── Map runtime stubs (tk_map_new / tk_map_put / tk_map_get) ────────── */
/*
 * Simple string-keyed map for toke programs.  Keys and values are i64
 * (pointer-as-integer for strings, raw i64 for integers).
 * Key comparison uses strcmp on the pointer interpreted as char *.
 */
typedef struct { int64_t key; int64_t val; } TkMapEntry;
typedef struct { TkMapEntry *entries; int len; int cap; } TkMapImpl;

void *tk_map_new(void) {
    TkMapImpl *m = (TkMapImpl *)calloc(1, sizeof(TkMapImpl));
    return m;
}

void tk_map_put(void *m_ptr, int64_t key, int64_t val) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return;
    const char *ks = (const char *)(intptr_t)key;
    for (int i = 0; i < m->len; i++) {
        const char *ki = (const char *)(intptr_t)m->entries[i].key;
        if ((ks && ki && strcmp(ks, ki) == 0) || (!ks && !ki)) {
            m->entries[i].val = val;
            return;
        }
    }
    if (m->len >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        TkMapEntry *ne = (TkMapEntry *)realloc(m->entries, (size_t)nc * sizeof(TkMapEntry));
        if (!ne) return;
        m->entries = ne; m->cap = nc;
    }
    m->entries[m->len].key = key;
    m->entries[m->len].val = val;
    m->len++;
}

int64_t tk_map_get(void *m_ptr, int64_t key) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return 0;
    const char *ks = (const char *)(intptr_t)key;
    for (int i = 0; i < m->len; i++) {
        const char *ki = (const char *)(intptr_t)m->entries[i].key;
        if ((ks && ki && strcmp(ks, ki) == 0) || (!ks && !ki))
            return m->entries[i].val;
    }
    return 0;
}

/* ── Array/map instance method wrappers (Story 56.8.7) ───────────────── */

/*
 * tk_array_append_w — Append an element to a toke array and return the new array.
 *
 * toke array layout: ptr[-1] = length (i64), ptr[0..len-1] = elements (i64).
 */
int64_t tk_array_append_w(int64_t arr_i64, int64_t elem) {
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    int64_t *block = (int64_t *)malloc((size_t)(len + 2) * sizeof(int64_t));
    if (!block) return arr_i64;
    block[0] = len + 1;
    if (len > 0) memcpy(block + 1, ptr, (size_t)len * sizeof(int64_t));
    block[len + 1] = elem;
    return (int64_t)(intptr_t)(block + 1);
}

/*
 * tk_map_set_w — Set a key/value pair in a toke map and return the map.
 */
int64_t tk_map_set_w(int64_t map_i64, int64_t key, int64_t val) {
    tk_map_put((void *)(intptr_t)map_i64, key, val);
    return map_i64;
}

/* ── Missing stdlib wrappers (Story 56.8.7) ──────────────────────────── */

int64_t tk_str_contains_w(int64_t s, int64_t sub) {
    const char *cs = (const char *)(intptr_t)s;
    const char *csub = (const char *)(intptr_t)sub;
    if (!cs || !csub) return 0;
    return str_contains(cs, csub) ? 1 : 0;
}

int64_t tk_path_ext_w(int64_t p) {
    if (!p) return 0;
    return (int64_t)(intptr_t)path_ext((const char *)(intptr_t)p);
}

int64_t tk_md_render_w(int64_t src) {
    if (!src) return 0;
    return (int64_t)(intptr_t)md_render((const char *)(intptr_t)src);
}
