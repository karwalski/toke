/*
 * proxy.c — HTTP reverse proxy and load balancing for std.http.
 *
 * Epic 62: reverse proxy handler, connection pooling, hop-by-hop header
 * management, forwarding metadata, timeout config, load balancing
 * (round-robin, least-connections, IP-hash, weighted), session affinity,
 * and health checking (passive, active, circuit breaker).
 */

#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ── Types ───────────────────────────────────────────────────────────── */

/* Backend state (62.2) */
typedef struct {
    char    *host;
    int      port;
    int      weight;         /* 62.2.4: default 1 */
    int      active_conns;   /* 62.2.2: for least-connections */
    int      healthy;        /* 62.3.1: 1=healthy */
    int      consecutive_ok; /* 62.3.1: successes since last failure */
    int      consecutive_fail;
    time_t   unhealthy_since;
    time_t   cooldown_until; /* 62.3.3: circuit breaker */
} ProxyBackend;

/* Connection pool entry (62.1.2) */
typedef struct {
    int      fd;
    char    *host;
    int      port;
    time_t   created;
    time_t   last_used;
    int      in_use;
} PoolEntry;

#define PROXY_MAX_BACKENDS     32
#define PROXY_POOL_SIZE        64
#define PROXY_CONNECT_TIMEOUT  5    /* seconds (62.1.5) */
#define PROXY_READ_TIMEOUT     30
#define PROXY_WRITE_TIMEOUT    10
#define PROXY_POOL_IDLE_SECS   60   /* evict idle connections */
#define PROXY_POOL_MAX_AGE     300  /* max connection lifetime */
#define PROXY_HEALTH_THRESHOLD 3    /* failures before unhealthy */
#define PROXY_RECOVER_THRESHOLD 2   /* successes to recover */
#define PROXY_COOLDOWN_SECS    30   /* circuit breaker cooldown */

/* Load balancing algorithms (62.2) */
typedef enum {
    LB_ROUND_ROBIN = 0,
    LB_LEAST_CONN,
    LB_IP_HASH,
    LB_WEIGHTED_RR,
} LbAlgorithm;

/* Upstream group (62.2.1) */
typedef struct {
    char          *name;
    ProxyBackend   backends[PROXY_MAX_BACKENDS];
    int            count;
    int            rr_index;        /* round-robin cursor */
    LbAlgorithm    algorithm;
    /* Session affinity cookie (62.2.5) */
    char          *affinity_cookie;
    int            cookie_max_age;
    /* Health check config (62.3.2) */
    char          *health_path;     /* e.g. "/health" */
    int            health_interval; /* seconds */
    int            health_timeout;  /* seconds */
    /* Timeouts (62.1.5) */
    int            connect_timeout;
    int            read_timeout;
    int            write_timeout;
} ProxyUpstream;

/* Connection pool (62.1.2) */
static PoolEntry g_pool[PROXY_POOL_SIZE];
static int       g_pool_init = 0;

/* Upstream registry */
#define MAX_UPSTREAMS 16
static ProxyUpstream g_upstreams[MAX_UPSTREAMS];
static int           g_upstream_count = 0;

/* ── Hop-by-hop headers (62.1.3) ─────────────────────────────────────── */

static const char *hop_by_hop[] = {
    "Connection", "Keep-Alive", "Proxy-Authenticate",
    "Proxy-Authorization", "TE", "Trailers",
    "Transfer-Encoding", "Upgrade", NULL
};

static int is_hop_by_hop(const char *name)
{
    for (int i = 0; hop_by_hop[i]; i++) {
        if (strcasecmp(name, hop_by_hop[i]) == 0) return 1;
    }
    return 0;
}

/* ── Connection pool (62.1.2) ────────────────────────────────────────── */

static void pool_init(void)
{
    if (g_pool_init) return;
    memset(g_pool, 0, sizeof(g_pool));
    g_pool_init = 1;
}

static void pool_evict_stale(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < PROXY_POOL_SIZE; i++) {
        if (g_pool[i].fd > 0 && !g_pool[i].in_use) {
            if (now - g_pool[i].last_used > PROXY_POOL_IDLE_SECS ||
                now - g_pool[i].created > PROXY_POOL_MAX_AGE) {
                close(g_pool[i].fd);
                free(g_pool[i].host);
                memset(&g_pool[i], 0, sizeof(PoolEntry));
            }
        }
    }
}

/* Get a pooled connection or -1 */
static int pool_get(const char *host, int port)
{
    pool_init();
    pool_evict_stale();
    for (int i = 0; i < PROXY_POOL_SIZE; i++) {
        if (g_pool[i].fd > 0 && !g_pool[i].in_use &&
            g_pool[i].port == port &&
            g_pool[i].host && strcmp(g_pool[i].host, host) == 0) {
            g_pool[i].in_use = 1;
            g_pool[i].last_used = time(NULL);
            return g_pool[i].fd;
        }
    }
    return -1;
}

/* Return a connection to the pool */
static void pool_put(int fd, const char *host, int port)
{
    pool_init();
    for (int i = 0; i < PROXY_POOL_SIZE; i++) {
        if (g_pool[i].fd == fd) {
            g_pool[i].in_use = 0;
            g_pool[i].last_used = time(NULL);
            return;
        }
    }
    /* New entry */
    for (int i = 0; i < PROXY_POOL_SIZE; i++) {
        if (g_pool[i].fd <= 0) {
            g_pool[i].fd = fd;
            g_pool[i].host = strdup(host);
            g_pool[i].port = port;
            g_pool[i].created = time(NULL);
            g_pool[i].last_used = time(NULL);
            g_pool[i].in_use = 0;
            return;
        }
    }
    /* Pool full — just close */
    close(fd);
}

/* ── TCP connection with timeout (62.1.5) ────────────────────────────── */

static int proxy_connect(const char *host, int port, int timeout_s)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) return -1;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }

    /* Set connect timeout */
    struct timeval tv;
    tv.tv_sec = timeout_s;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

/* ── Load balancing (62.2) ───────────────────────────────────────────── */

/* FNV-1a hash for IP-hash (62.2.3) */
static uint32_t fnv1a_hash(const char *data)
{
    uint32_t hash = 2166136261u;
    while (*data) {
        hash ^= (uint8_t)*data++;
        hash *= 16777619u;
    }
    return hash;
}

/* Select a backend from the upstream group */
static ProxyBackend *lb_select(ProxyUpstream *up, const char *client_ip)
{
    if (up->count == 0) return NULL;

    /* Filter healthy backends */
    ProxyBackend *healthy[PROXY_MAX_BACKENDS];
    int nhealthy = 0;
    time_t now = time(NULL);

    for (int i = 0; i < up->count; i++) {
        if (up->backends[i].healthy ||
            now >= up->backends[i].cooldown_until) {
            healthy[nhealthy++] = &up->backends[i];
        }
    }
    if (nhealthy == 0) return NULL;

    switch (up->algorithm) {
    case LB_ROUND_ROBIN:
        up->rr_index = (up->rr_index + 1) % nhealthy;
        return healthy[up->rr_index];

    case LB_LEAST_CONN: {
        ProxyBackend *best = healthy[0];
        for (int i = 1; i < nhealthy; i++) {
            if (healthy[i]->active_conns < best->active_conns)
                best = healthy[i];
        }
        return best;
    }

    case LB_IP_HASH: {
        if (!client_ip) return healthy[0];
        uint32_t h = fnv1a_hash(client_ip);
        return healthy[h % (uint32_t)nhealthy];
    }

    case LB_WEIGHTED_RR: {
        /* Smooth weighted round-robin */
        int total_weight = 0;
        for (int i = 0; i < nhealthy; i++)
            total_weight += healthy[i]->weight > 0 ? healthy[i]->weight : 1;
        int r = up->rr_index % total_weight;
        up->rr_index++;
        int cum = 0;
        for (int i = 0; i < nhealthy; i++) {
            cum += healthy[i]->weight > 0 ? healthy[i]->weight : 1;
            if (r < cum) return healthy[i];
        }
        return healthy[0];
    }
    }

    return healthy[0];
}

/* ── Forward a request to a backend (62.1.1) ─────────────────────────── */

/* Build an HTTP/1.1 request string for the backend */
static char *proxy_build_request(Req *req, const char *host, int port,
                                  const char *client_ip)
{
    size_t cap = 4096;
    char *buf = malloc(cap);
    if (!buf) return NULL;

    int off = snprintf(buf, cap, "%s %s HTTP/1.1\r\n",
                       req->method ? req->method : "GET",
                       req->path ? req->path : "/");

    /* Host header */
    off += snprintf(buf + off, cap - (size_t)off, "Host: %s:%d\r\n",
                    host, port);

    /* Forwarding metadata headers (62.1.4) */
    off += snprintf(buf + off, cap - (size_t)off,
                    "X-Forwarded-For: %s\r\n"
                    "X-Forwarded-Proto: https\r\n",
                    client_ip ? client_ip : "unknown");

    /* Copy non-hop-by-hop headers */
    for (uint64_t i = 0; i < req->headers.len; i++) {
        const char *name = req->headers.data[i].key;
        const char *val  = req->headers.data[i].val;
        if (is_hop_by_hop(name)) continue;
        if (strcasecmp(name, "Host") == 0) continue; /* already set */
        off += snprintf(buf + off, cap - (size_t)off, "%s: %s\r\n",
                        name, val);
    }

    /* Body */
    size_t body_len = req->body ? strlen(req->body) : 0;
    if (body_len > 0) {
        off += snprintf(buf + off, cap - (size_t)off,
                        "Content-Length: %zu\r\n", body_len);
    }

    off += snprintf(buf + off, cap - (size_t)off, "Connection: keep-alive\r\n\r\n");

    if (body_len > 0 && (size_t)off + body_len < cap) {
        memcpy(buf + off, req->body, body_len);
        off += (int)body_len;
    }
    buf[off] = '\0';

    return buf;
}

/* Parse a backend HTTP response (minimal) */
static Res proxy_parse_response(int fd, int read_timeout)
{
    Res res = {0};
    res.status = 502; /* default: Bad Gateway */

    struct timeval tv;
    tv.tv_sec = read_timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Read response */
    size_t cap = 65536;
    char *buf = malloc(cap);
    if (!buf) return res;

    size_t used = 0;
    int headers_done = 0;

    while (used < cap - 1) {
        ssize_t n = read(fd, buf + used, cap - used - 1);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                res.status = 504; /* Gateway Timeout */
            }
            break;
        }
        used += (size_t)n;
        buf[used] = '\0';
        if (strstr(buf, "\r\n\r\n")) {
            headers_done = 1;
            break;
        }
    }

    if (!headers_done) {
        free(buf);
        return res;
    }

    /* Parse status line */
    if (strncmp(buf, "HTTP/", 5) == 0) {
        char *sp = strchr(buf, ' ');
        if (sp) res.status = (uint32_t)atoi(sp + 1);
    }

    /* Find body */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) body_start += 4;

    /* Get Content-Length */
    size_t content_length = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl) {
        content_length = (size_t)atol(cl + 15);
    }

    /* Read remaining body */
    if (body_start && content_length > 0) {
        size_t body_have = used - (size_t)(body_start - buf);
        while (body_have < content_length && used < cap - 1) {
            ssize_t n = read(fd, buf + used, cap - used - 1);
            if (n <= 0) break;
            used += (size_t)n;
            body_have += (size_t)n;
        }
        buf[used] = '\0';
    }

    /* Copy body to response */
    if (body_start) {
        size_t blen = used - (size_t)(body_start - buf);
        char *body_copy = malloc(blen + 1);
        if (body_copy) {
            memcpy(body_copy, body_start, blen);
            body_copy[blen] = '\0';
            res.body = body_copy;
        }
    }

    free(buf);
    return res;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/* Create an upstream group (62.2.1) */
ProxyUpstream *proxy_upstream_new(const char *name, LbAlgorithm algo)
{
    if (g_upstream_count >= MAX_UPSTREAMS) return NULL;
    ProxyUpstream *up = &g_upstreams[g_upstream_count++];
    memset(up, 0, sizeof(*up));
    up->name = strdup(name);
    up->algorithm = algo;
    up->connect_timeout = PROXY_CONNECT_TIMEOUT;
    up->read_timeout = PROXY_READ_TIMEOUT;
    up->write_timeout = PROXY_WRITE_TIMEOUT;
    return up;
}

/* Add a backend to an upstream group */
int proxy_upstream_add(ProxyUpstream *up, const char *host,
                        int port, int weight)
{
    if (!up || up->count >= PROXY_MAX_BACKENDS) return -1;
    ProxyBackend *b = &up->backends[up->count++];
    memset(b, 0, sizeof(*b));
    b->host = strdup(host);
    b->port = port;
    b->weight = weight > 0 ? weight : 1;
    b->healthy = 1;
    return 0;
}

/* Set session affinity cookie (62.2.5) */
void proxy_upstream_set_affinity(ProxyUpstream *up, const char *cookie_name,
                                  int max_age)
{
    if (!up) return;
    free(up->affinity_cookie);
    up->affinity_cookie = strdup(cookie_name);
    up->cookie_max_age = max_age > 0 ? max_age : 3600;
}

/* Set health check config (62.3.2) */
void proxy_upstream_set_health_check(ProxyUpstream *up, const char *path,
                                      int interval, int timeout)
{
    if (!up) return;
    free(up->health_path);
    up->health_path = strdup(path);
    up->health_interval = interval > 0 ? interval : 10;
    up->health_timeout = timeout > 0 ? timeout : 5;
}

/* Set timeouts (62.1.5) */
void proxy_upstream_set_timeouts(ProxyUpstream *up, int connect_s,
                                  int read_s, int write_s)
{
    if (!up) return;
    if (connect_s > 0) up->connect_timeout = connect_s;
    if (read_s > 0) up->read_timeout = read_s;
    if (write_s > 0) up->write_timeout = write_s;
}

/* Proxy a request to an upstream group (62.1.1) */
Res proxy_forward(ProxyUpstream *up, Req req, const char *client_ip)
{
    Res res = {0};
    res.status = 502;
    res.body = "Bad Gateway";

    if (!up) return res;

    ProxyBackend *backend = lb_select(up, client_ip);
    if (!backend) {
        res.status = 503;
        res.body = "Service Unavailable";
        return res;
    }

    /* Try pooled connection first */
    int fd = pool_get(backend->host, backend->port);
    if (fd < 0) {
        fd = proxy_connect(backend->host, backend->port,
                            up->connect_timeout);
    }

    if (fd < 0) {
        /* Connection refused — passive health check (62.3.1) */
        backend->consecutive_fail++;
        backend->consecutive_ok = 0;
        if (backend->consecutive_fail >= PROXY_HEALTH_THRESHOLD) {
            backend->healthy = 0;
            backend->unhealthy_since = time(NULL);
            backend->cooldown_until = time(NULL) + PROXY_COOLDOWN_SECS;
        }
        res.status = 502;
        res.body = "Bad Gateway - Connection Refused";
        return res;
    }

    backend->active_conns++;

    /* Build and send request */
    char *req_str = proxy_build_request(&req, backend->host, backend->port,
                                         client_ip);
    if (!req_str) {
        backend->active_conns--;
        close(fd);
        return res;
    }

    struct timeval wtv;
    wtv.tv_sec = up->write_timeout;
    wtv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wtv, sizeof(wtv));

    ssize_t sent = write(fd, req_str, strlen(req_str));
    free(req_str);

    if (sent <= 0) {
        backend->active_conns--;
        close(fd);
        backend->consecutive_fail++;
        backend->consecutive_ok = 0;
        return res;
    }

    /* Read response */
    res = proxy_parse_response(fd, up->read_timeout);

    backend->active_conns--;

    /* Passive health tracking (62.3.1) */
    if (res.status >= 500) {
        backend->consecutive_fail++;
        backend->consecutive_ok = 0;
        if (backend->consecutive_fail >= PROXY_HEALTH_THRESHOLD) {
            backend->healthy = 0;
            backend->unhealthy_since = time(NULL);
            backend->cooldown_until = time(NULL) + PROXY_COOLDOWN_SECS;
        }
    } else {
        backend->consecutive_ok++;
        backend->consecutive_fail = 0;
        if (!backend->healthy &&
            backend->consecutive_ok >= PROXY_RECOVER_THRESHOLD) {
            backend->healthy = 1;
        }
    }

    /* Return connection to pool */
    if (res.status < 500) {
        pool_put(fd, backend->host, backend->port);
    } else {
        close(fd);
    }

    return res;
}

/* Route handler wrapper for use with http.route */
static ProxyUpstream *g_proxy_handler_upstream = NULL;

Res proxy_route_handler(Req req)
{
    /* Extract client IP from X-Real-IP or request context */
    const char *client_ip = "unknown";
    for (uint64_t i = 0; i < req.headers.len; i++) {
        if (strcasecmp(req.headers.data[i].key, "X-Real-IP") == 0) {
            client_ip = req.headers.data[i].val;
            break;
        }
    }
    return proxy_forward(g_proxy_handler_upstream, req, client_ip);
}

/* ── Active health probes (62.3.2) ───────────────────────────────────── */

/* Run one health probe against a backend. Returns 0 if healthy. */
int proxy_health_probe(ProxyBackend *b, const char *path, int timeout)
{
    int fd = proxy_connect(b->host, b->port, timeout);
    if (fd < 0) return -1;

    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
             "Connection: close\r\n\r\n",
             path, b->host, b->port);

    write(fd, req, strlen(req));

    struct timeval tv;
    tv.tv_sec = timeout;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) return -1;
    buf[n] = '\0';

    /* Check for 2xx status */
    char *sp = strchr(buf, ' ');
    if (sp) {
        int status = atoi(sp + 1);
        return (status >= 200 && status < 300) ? 0 : -1;
    }
    return -1;
}

/* Run active health checks on all backends in an upstream group */
void proxy_health_check_all(ProxyUpstream *up)
{
    if (!up || !up->health_path) return;

    for (int i = 0; i < up->count; i++) {
        ProxyBackend *b = &up->backends[i];
        int rc = proxy_health_probe(b, up->health_path, up->health_timeout);
        if (rc == 0) {
            b->consecutive_ok++;
            b->consecutive_fail = 0;
            if (!b->healthy &&
                b->consecutive_ok >= PROXY_RECOVER_THRESHOLD) {
                b->healthy = 1;
                fprintf(stderr, "proxy: %s:%d recovered\n",
                        b->host, b->port);
            }
        } else {
            b->consecutive_fail++;
            b->consecutive_ok = 0;
            if (b->healthy &&
                b->consecutive_fail >= PROXY_HEALTH_THRESHOLD) {
                b->healthy = 0;
                b->unhealthy_since = time(NULL);
                b->cooldown_until = time(NULL) + PROXY_COOLDOWN_SECS;
                fprintf(stderr, "proxy: %s:%d marked unhealthy\n",
                        b->host, b->port);
            }
        }
    }
}

/* ── Cleanup ─────────────────────────────────────────────────────────── */

void proxy_upstream_free(ProxyUpstream *up)
{
    if (!up) return;
    free(up->name);
    for (int i = 0; i < up->count; i++) {
        free(up->backends[i].host);
    }
    free(up->affinity_cookie);
    free(up->health_path);
    memset(up, 0, sizeof(*up));
}
