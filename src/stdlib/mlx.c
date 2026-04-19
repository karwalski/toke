/*
 * mlx.c — Implementation of the std.mlx standard library module.
 *
 * Implements a REST client that communicates with a local MLX bridge process
 * listening on localhost:MLX_BRIDGE_PORT (default 11438).  All HTTP calls
 * use raw POSIX sockets; no external HTTP library is required.
 *
 * Apple Silicon detection uses sysctl(hw.optional.arm64).  The entire
 * implementation is guarded by #ifdef __APPLE__ so the file compiles cleanly
 * on Linux, Windows (MinGW), and any other platform — all functions become
 * no-op stubs that return safe error values.
 *
 * malloc is permitted; callers own all returned heap pointers.
 *
 * No external dependencies beyond libc and POSIX sockets.
 *
 * Story: 72.6.3
 */

#include "mlx.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* =========================================================================
 * Platform-independent helpers
 * ========================================================================= */

/* strdup_mlx — portable strdup; returns NULL on NULL input or alloc failure. */
static char *strdup_mlx(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* =========================================================================
 * Memory management
 * ========================================================================= */

void mlx_model_free(MlxModel *m)
{
    if (!m) return;
    free(m->id);
    free(m->path);
    m->id   = NULL;
    m->path = NULL;
}

void mlx_str_result_free(MlxStrResult *r)
{
    if (!r) return;
    free(r->text);
    free(r->err_msg);
    r->text    = NULL;
    r->err_msg = NULL;
}

void mlx_embed_result_free(MlxEmbedResult *r)
{
    if (!r) return;
    free(r->embedding.data);
    free(r->err_msg);
    r->embedding.data = NULL;
    r->embedding.len  = 0;
    r->err_msg        = NULL;
}

/* =========================================================================
 * Apple Silicon implementation
 * ========================================================================= */

#ifdef __APPLE__

#include <sys/sysctl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * Apple Silicon detection
 * ------------------------------------------------------------------------- */

/*
 * is_apple_silicon — return 1 if hw.optional.arm64 == 1, else 0.
 * Cached after first call.
 */
static int is_apple_silicon(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;

    int val = 0;
    size_t sz = sizeof(val);
    if (sysctlbyname("hw.optional.arm64", &val, &sz, NULL, 0) == 0 && val == 1) {
        cached = 1;
    } else {
        cached = 0;
    }
    return cached;
}

/* -------------------------------------------------------------------------
 * TCP connect to 127.0.0.1:MLX_BRIDGE_PORT
 * ------------------------------------------------------------------------- */

/*
 * bridge_connect — open a TCP socket to 127.0.0.1 on MLX_BRIDGE_PORT.
 * Returns a connected fd >= 0, or -1 on failure.
 */
static int bridge_connect(void)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", MLX_BRIDGE_PORT);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo("127.0.0.1", port_str, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* -------------------------------------------------------------------------
 * HTTP GET helper
 * ------------------------------------------------------------------------- */

/*
 * bridge_get — perform GET /path on the bridge.
 * Returns the HTTP status code, or -1 on socket error.
 * Discards the response body.
 */
static int bridge_get(const char *path)
{
    int fd = bridge_connect();
    if (fd < 0) return -1;

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n",
        path);

    if (req_len < 0 || (size_t)req_len >= sizeof(req)) {
        close(fd);
        return -1;
    }

    if (send(fd, req, (size_t)req_len, 0) < 0) {
        close(fd);
        return -1;
    }

    /* read just enough to extract the status line */
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);

    if (n <= 0) return -1;
    buf[n] = '\0';

    /* "HTTP/1.1 200 ..." */
    int status = -1;
    if (strncmp(buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf, ' ');
        if (sp) status = (int)strtol(sp + 1, NULL, 10);
    }
    return status;
}

/* -------------------------------------------------------------------------
 * HTTP POST helper
 * ------------------------------------------------------------------------- */

/*
 * bridge_post — POST json_body to /path on the bridge.
 * On success returns a heap-allocated NUL-terminated string containing the
 * response body (caller must free).  *http_status receives the HTTP status
 * code.  Returns NULL on socket or protocol error.
 */
static char *bridge_post(const char *path, const char *json_body,
                          int *http_status)
{
    int fd = bridge_connect();
    if (fd < 0) {
        *http_status = -1;
        return NULL;
    }

    size_t body_len = json_body ? strlen(json_body) : 0;

    char req_hdr[512];
    int hdr_len = snprintf(req_hdr, sizeof(req_hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, body_len);

    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(req_hdr)) {
        close(fd);
        *http_status = -1;
        return NULL;
    }

    if (send(fd, req_hdr, (size_t)hdr_len, 0) < 0) {
        close(fd);
        *http_status = -1;
        return NULL;
    }
    if (json_body && body_len > 0) {
        if (send(fd, json_body, body_len, 0) < 0) {
            close(fd);
            *http_status = -1;
            return NULL;
        }
    }

    /* read full response */
    size_t cap = 16384, used = 0;
    char *resp = (char *)malloc(cap);
    if (!resp) { close(fd); *http_status = -1; return NULL; }

    ssize_t n;
    while ((n = recv(fd, resp + used, cap - used - 1, 0)) > 0) {
        used += (size_t)n;
        if (used + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(resp, cap);
            if (!tmp) { free(resp); close(fd); *http_status = -1; return NULL; }
            resp = tmp;
        }
    }
    resp[used] = '\0';
    close(fd);

    /* extract status code */
    *http_status = -1;
    if (strncmp(resp, "HTTP/", 5) == 0) {
        const char *sp = strchr(resp, ' ');
        if (sp) *http_status = (int)strtol(sp + 1, NULL, 10);
    }

    /* split off body (after \r\n\r\n) */
    const char *body_start = strstr(resp, "\r\n\r\n");
    if (!body_start) {
        free(resp);
        return NULL;
    }
    body_start += 4;

    char *body = strdup_mlx(body_start);
    free(resp);
    return body;
}

/* -------------------------------------------------------------------------
 * Minimal JSON extraction helpers
 *
 * These parse only the subset of JSON emitted by the MLX bridge:
 *   - string values:   "key":"value"
 *   - number arrays:   "key":[n,n,n,...]
 *   - error strings:   "error":"message"
 *
 * They do NOT handle nested objects, escaped characters in keys, or any
 * other advanced JSON features.  A proper JSON library would be used in
 * production; these helpers keep the module self-contained.
 * ------------------------------------------------------------------------- */

/*
 * json_str_field — find "key":"value" in json and return a heap copy of
 * value, or NULL if not found.
 */
static char *json_str_field(const char *json, const char *key)
{
    if (!json || !key) return NULL;

    /* build search pattern: "key":" */
    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    if (plen < 0 || (size_t)plen >= sizeof(pattern)) return NULL;

    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += (size_t)plen;

    /* find closing quote (no escape handling) */
    const char *end = strchr(p, '"');
    if (!end) return NULL;

    size_t vlen = (size_t)(end - p);
    char *val = (char *)malloc(vlen + 1);
    if (!val) return NULL;
    memcpy(val, p, vlen);
    val[vlen] = '\0';
    return val;
}

/*
 * json_float_array — parse "key":[f,f,f,...] and return a heap-allocated
 * float array.  *out_len receives the number of elements.
 * Returns NULL on parse failure.
 */
static float *json_float_array(const char *json, const char *key,
                                uint64_t *out_len)
{
    *out_len = 0;
    if (!json || !key) return NULL;

    char pattern[128];
    int plen = snprintf(pattern, sizeof(pattern), "\"%s\":[", key);
    if (plen < 0 || (size_t)plen >= sizeof(pattern)) return NULL;

    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += (size_t)plen;

    /* find closing bracket */
    const char *end = strchr(p, ']');
    if (!end) return NULL;

    /* count commas to pre-allocate */
    size_t count = 1;
    for (const char *q = p; q < end; q++) {
        if (*q == ',') count++;
    }
    /* handle empty array */
    if (p == end || *p == ']') {
        *out_len = 0;
        return (float *)malloc(sizeof(float)); /* minimal allocation */
    }

    float *arr = (float *)malloc(count * sizeof(float));
    if (!arr) return NULL;

    size_t idx = 0;
    const char *cur = p;
    char *nxt;
    while (cur < end && idx < count) {
        arr[idx++] = (float)strtod(cur, &nxt);
        if (nxt == cur) break; /* no progress */
        cur = nxt;
        /* skip comma and whitespace */
        while (cur < end && (*cur == ',' || *cur == ' ' || *cur == '\n' ||
               *cur == '\r' || *cur == '\t')) {
            cur++;
        }
    }
    *out_len = idx;
    return arr;
}

/* -------------------------------------------------------------------------
 * API implementation (Apple Silicon)
 * ------------------------------------------------------------------------- */

int mlx_is_available(void)
{
    if (!is_apple_silicon()) return 0;
    int status = bridge_get("/health");
    return (status == 200) ? 1 : 0;
}

MlxModelResult mlx_load(const char *model_path)
{
    MlxModelResult r;
    memset(&r, 0, sizeof(r));

    if (!is_apple_silicon()) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.load: not available on this platform");
        return r;
    }
    if (!model_path || model_path[0] == '\0') {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.load: model_path must not be empty");
        return r;
    }

    /* build request JSON */
    size_t path_len = strlen(model_path);
    size_t req_cap  = path_len + 32;
    char *req = (char *)malloc(req_cap);
    if (!req) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.load: out of memory");
        return r;
    }
    snprintf(req, req_cap, "{\"model_path\":\"%s\"}", model_path);

    int status = -1;
    char *body = bridge_post("/load", req, &status);
    free(req);

    if (!body) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.load: bridge connection failed");
        return r;
    }

    if (status != 200) {
        char *errmsg = json_str_field(body, "error");
        r.is_err  = 1;
        r.err_msg = errmsg ? errmsg : strdup_mlx("mlx.load: bridge returned error");
        free(body);
        return r;
    }

    char *id   = json_str_field(body, "id");
    char *path = json_str_field(body, "path");
    free(body);

    if (!id || !path) {
        free(id);
        free(path);
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.load: unexpected bridge response (missing id or path)");
        return r;
    }

    r.model.id   = id;
    r.model.path = path;
    return r;
}

int mlx_unload(const MlxModel *m)
{
    if (!m || !m->id) return 0;
    if (!is_apple_silicon()) return 0;

    size_t id_len = strlen(m->id);
    size_t req_cap = id_len + 16;
    char *req = (char *)malloc(req_cap);
    if (!req) return 0;
    snprintf(req, req_cap, "{\"id\":\"%s\"}", m->id);

    int status = -1;
    char *body = bridge_post("/unload", req, &status);
    free(req);
    free(body);

    return (status == 200) ? 1 : 0;
}

MlxStrResult mlx_generate(const MlxModel *m, const char *prompt,
                           int32_t max_tokens)
{
    MlxStrResult r;
    memset(&r, 0, sizeof(r));

    if (!is_apple_silicon()) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.generate: not available on this platform");
        return r;
    }
    if (!m || !m->id) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.generate: invalid model handle");
        return r;
    }
    if (!prompt) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.generate: prompt must not be NULL");
        return r;
    }

    size_t id_len     = strlen(m->id);
    size_t prompt_len = strlen(prompt);
    /* extra bytes: {"id":"","prompt":"","max_tokens":2147483647} = ~40 chars */
    size_t req_cap = id_len + prompt_len + 64;
    char *req = (char *)malloc(req_cap);
    if (!req) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.generate: out of memory");
        return r;
    }
    snprintf(req, req_cap,
             "{\"id\":\"%s\",\"prompt\":\"%s\",\"max_tokens\":%d}",
             m->id, prompt, (int)max_tokens);

    int status = -1;
    char *body = bridge_post("/generate", req, &status);
    free(req);

    if (!body) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.generate: bridge connection failed");
        return r;
    }

    if (status != 200) {
        char *errmsg = json_str_field(body, "error");
        r.is_err  = 1;
        r.err_msg = errmsg ? errmsg
                           : strdup_mlx("mlx.generate: bridge returned error");
        free(body);
        return r;
    }

    char *text = json_str_field(body, "text");
    free(body);

    if (!text) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.generate: unexpected bridge response (missing text)");
        return r;
    }

    r.text = text;
    return r;
}

MlxEmbedResult mlx_embed(const MlxModel *m, const char *text)
{
    MlxEmbedResult r;
    memset(&r, 0, sizeof(r));

    if (!is_apple_silicon()) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.embed: not available on this platform");
        return r;
    }
    if (!m || !m->id) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.embed: invalid model handle");
        return r;
    }
    if (!text) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.embed: text must not be NULL");
        return r;
    }

    size_t id_len   = strlen(m->id);
    size_t text_len = strlen(text);
    size_t req_cap  = id_len + text_len + 24;
    char *req = (char *)malloc(req_cap);
    if (!req) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.embed: out of memory");
        return r;
    }
    snprintf(req, req_cap, "{\"id\":\"%s\",\"text\":\"%s\"}", m->id, text);

    int status = -1;
    char *body = bridge_post("/embed", req, &status);
    free(req);

    if (!body) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.embed: bridge connection failed");
        return r;
    }

    if (status != 200) {
        char *errmsg = json_str_field(body, "error");
        r.is_err  = 1;
        r.err_msg = errmsg ? errmsg
                           : strdup_mlx("mlx.embed: bridge returned error");
        free(body);
        return r;
    }

    uint64_t arr_len = 0;
    float *arr = json_float_array(body, "embedding", &arr_len);
    free(body);

    if (!arr) {
        r.is_err  = 1;
        r.err_msg = strdup_mlx("mlx.embed: failed to parse embedding array");
        return r;
    }

    r.embedding.data = arr;
    r.embedding.len  = arr_len;
    return r;
}

#else /* !__APPLE__ */

/* =========================================================================
 * Non-Apple stub implementations — all functions are safe no-ops
 * ========================================================================= */

int mlx_is_available(void)
{
    return 0;
}

MlxModelResult mlx_load(const char *model_path)
{
    MlxModelResult r;
    memset(&r, 0, sizeof(r));
    (void)model_path;
    r.is_err  = 1;
    r.err_msg = strdup_mlx("mlx.load: MLX is only available on Apple Silicon");
    return r;
}

int mlx_unload(const MlxModel *m)
{
    (void)m;
    return 0;
}

MlxStrResult mlx_generate(const MlxModel *m, const char *prompt,
                           int32_t max_tokens)
{
    MlxStrResult r;
    memset(&r, 0, sizeof(r));
    (void)m; (void)prompt; (void)max_tokens;
    r.is_err  = 1;
    r.err_msg = strdup_mlx("mlx.generate: MLX is only available on Apple Silicon");
    return r;
}

MlxEmbedResult mlx_embed(const MlxModel *m, const char *text)
{
    MlxEmbedResult r;
    memset(&r, 0, sizeof(r));
    (void)m; (void)text;
    r.is_err  = 1;
    r.err_msg = strdup_mlx("mlx.embed: MLX is only available on Apple Silicon");
    return r;
}

#endif /* __APPLE__ */
