/*
 * cache.c — HTTP response cache for std.http.
 *
 * Epic 63: in-memory LRU cache, Cache-Control parsing, conditional requests,
 * Vary-aware keying, disk-backed tier, purge/invalidation, micro-caching,
 * stale-while-revalidate, stale-if-error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

/* ── Cache-Control directives (63.1.2) ───────────────────────────────── */

typedef struct {
    int  no_cache;
    int  no_store;
    int  public_dir;
    int  private_dir;
    int  must_revalidate;
    int  only_if_cached;
    long max_age;                /* -1 = not set */
    long s_maxage;               /* -1 = not set */
    long stale_while_revalidate; /* -1 = not set */
    long stale_if_error;         /* -1 = not set */
} CacheControl;

static CacheControl cache_control_parse(const char *header)
{
    CacheControl cc;
    memset(&cc, 0, sizeof(cc));
    cc.max_age = -1;
    cc.s_maxage = -1;
    cc.stale_while_revalidate = -1;
    cc.stale_if_error = -1;

    if (!header) return cc;

    const char *p = header;
    while (*p) {
        /* skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (*p == '\0') break;

        if (strncasecmp(p, "no-cache", 8) == 0) {
            cc.no_cache = 1; p += 8;
        } else if (strncasecmp(p, "no-store", 8) == 0) {
            cc.no_store = 1; p += 8;
        } else if (strncasecmp(p, "public", 6) == 0) {
            cc.public_dir = 1; p += 6;
        } else if (strncasecmp(p, "private", 7) == 0) {
            cc.private_dir = 1; p += 7;
        } else if (strncasecmp(p, "must-revalidate", 15) == 0) {
            cc.must_revalidate = 1; p += 15;
        } else if (strncasecmp(p, "only-if-cached", 14) == 0) {
            cc.only_if_cached = 1; p += 14;
        } else if (strncasecmp(p, "s-maxage=", 9) == 0) {
            cc.s_maxage = atol(p + 9); p += 9;
            while (*p >= '0' && *p <= '9') p++;
        } else if (strncasecmp(p, "max-age=", 8) == 0) {
            cc.max_age = atol(p + 8); p += 8;
            while (*p >= '0' && *p <= '9') p++;
        } else if (strncasecmp(p, "stale-while-revalidate=", 23) == 0) {
            cc.stale_while_revalidate = atol(p + 23); p += 23;
            while (*p >= '0' && *p <= '9') p++;
        } else if (strncasecmp(p, "stale-if-error=", 15) == 0) {
            cc.stale_if_error = atol(p + 15); p += 15;
            while (*p >= '0' && *p <= '9') p++;
        } else {
            /* skip unknown directive */
            while (*p && *p != ',') p++;
        }
    }
    return cc;
}

/* ── Cache entry ─────────────────────────────────────────────────────── */

typedef struct CacheEntry {
    char    *key;           /* composite key: method + URI + vary values */
    uint32_t status;
    char    *body;
    size_t   body_len;
    char    *etag;          /* for conditional revalidation (63.1.3) */
    char    *last_modified; /* for If-Modified-Since */
    char    *vary;          /* Vary header value (63.1.4) */
    char    *content_type;
    time_t   inserted;
    long     ttl;           /* seconds */
    time_t   last_access;
    struct CacheEntry *prev;
    struct CacheEntry *next;
} CacheEntry;

/* ── LRU cache (63.1.1) ─────────────────────────────────────────────── */

typedef struct {
    CacheEntry *head;       /* most recently used */
    CacheEntry *tail;       /* least recently used */
    size_t      count;
    size_t      max_entries;
    size_t      total_size;
    size_t      max_size;   /* bytes */
    /* Disk cache (63.1.6) */
    char       *disk_dir;
    size_t      disk_max_size;
    size_t      disk_used;
} HttpCache;

static HttpCache g_cache = {0};

/* ── Cache key construction (63.1.4 — Vary-aware) ────────────────────── */

/* Build a cache key from method + URI + Vary header values from request.
 * If vary is "Accept-Encoding", key includes the request's Accept-Encoding. */
static char *cache_build_key(const char *method, const char *uri,
                              const char *vary, const char *const *hdr_names,
                              const char *const *hdr_values, int nhdr)
{
    size_t cap = 1024;
    char *key = malloc(cap);
    if (!key) return NULL;

    int off = snprintf(key, cap, "%s|%s", method ? method : "GET",
                       uri ? uri : "/");

    /* Append Vary header values */
    if (vary && strcmp(vary, "*") != 0) {
        /* Parse comma-separated Vary header names */
        const char *p = vary;
        while (*p) {
            while (*p == ' ' || *p == ',') p++;
            if (*p == '\0') break;
            const char *name_start = p;
            while (*p && *p != ',') p++;
            size_t nlen = (size_t)(p - name_start);
            /* Trim trailing whitespace */
            while (nlen > 0 && (name_start[nlen-1] == ' ' ||
                                name_start[nlen-1] == '\t'))
                nlen--;

            /* Find this header in the request */
            for (int i = 0; i < nhdr; i++) {
                if (strncasecmp(hdr_names[i], name_start, nlen) == 0 &&
                    hdr_names[i][nlen] == '\0') {
                    off += snprintf(key + off, cap - (size_t)off,
                                    "|%s=%s", hdr_names[i], hdr_values[i]);
                    break;
                }
            }
        }
    }

    return key;
}

/* ── LRU operations ──────────────────────────────────────────────────── */

static void cache_unlink(CacheEntry *e)
{
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (g_cache.head == e) g_cache.head = e->next;
    if (g_cache.tail == e) g_cache.tail = e->prev;
    e->prev = e->next = NULL;
}

static void cache_push_front(CacheEntry *e)
{
    e->prev = NULL;
    e->next = g_cache.head;
    if (g_cache.head) g_cache.head->prev = e;
    g_cache.head = e;
    if (!g_cache.tail) g_cache.tail = e;
}

static void cache_entry_free(CacheEntry *e)
{
    if (!e) return;
    free(e->key);
    free(e->body);
    free(e->etag);
    free(e->last_modified);
    free(e->vary);
    free(e->content_type);
    g_cache.total_size -= e->body_len;
    g_cache.count--;
    free(e);
}

/* Evict LRU entries until under limits */
static void cache_evict(void)
{
    while (g_cache.tail &&
           (g_cache.count > g_cache.max_entries ||
            g_cache.total_size > g_cache.max_size)) {
        CacheEntry *victim = g_cache.tail;
        cache_unlink(victim);
        cache_entry_free(victim);
    }
}

/* ── Cache lookup ────────────────────────────────────────────────────── */

static CacheEntry *cache_find(const char *key)
{
    for (CacheEntry *e = g_cache.head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/* Initialise the cache (call once) */
void http_cache_init(size_t max_entries, size_t max_size_bytes)
{
    g_cache.max_entries = max_entries > 0 ? max_entries : 10000;
    g_cache.max_size = max_size_bytes > 0 ? max_size_bytes : (64 * 1024 * 1024);
}

/* Set disk cache directory (63.1.6) */
void http_cache_set_disk(const char *dir, size_t max_size_bytes)
{
    free(g_cache.disk_dir);
    g_cache.disk_dir = strdup(dir);
    g_cache.disk_max_size = max_size_bytes;
    g_cache.disk_used = 0;
    /* Create directory if needed */
    mkdir(dir, 0755);
}

/* Check cache for a response. Returns cache status string. */
const char *http_cache_get(const char *method, const char *uri,
                            const char *vary,
                            const char *const *req_hdr_names,
                            const char *const *req_hdr_values,
                            int req_nhdr,
                            uint32_t *out_status, const char **out_body,
                            size_t *out_body_len, const char **out_etag,
                            const char **out_content_type)
{
    char *key = cache_build_key(method, uri, vary,
                                 req_hdr_names, req_hdr_values, req_nhdr);
    if (!key) return "MISS";

    CacheEntry *e = cache_find(key);
    free(key);

    if (!e) return "MISS";

    time_t now = time(NULL);
    time_t age = now - e->inserted;

    if (age <= e->ttl) {
        /* Fresh hit */
        cache_unlink(e);
        cache_push_front(e);
        e->last_access = now;
        *out_status = e->status;
        *out_body = e->body;
        *out_body_len = e->body_len;
        *out_etag = e->etag;
        *out_content_type = e->content_type;
        return "HIT";
    }

    /* Expired — check stale-while-revalidate (63.2.3) */
    /* For now, return EXPIRED and let the caller revalidate */
    *out_status = e->status;
    *out_body = e->body;
    *out_body_len = e->body_len;
    *out_etag = e->etag;
    *out_content_type = e->content_type;
    return "EXPIRED";
}

/* Store a response in the cache */
void http_cache_put(const char *method, const char *uri,
                     uint32_t status, const char *body, size_t body_len,
                     const char *etag, const char *last_modified,
                     const char *vary, const char *content_type,
                     const char *cache_control,
                     const char *const *req_hdr_names,
                     const char *const *req_hdr_values,
                     int req_nhdr)
{
    /* Parse Cache-Control (63.1.2) */
    CacheControl cc = cache_control_parse(cache_control);

    /* Respect no-store */
    if (cc.no_store) return;

    /* Vary: * means never cache (63.1.4) */
    if (vary && strcmp(vary, "*") == 0) return;

    /* Only cache successful responses */
    if (status < 200 || status >= 400) return;

    /* Determine TTL */
    long ttl = 0;
    if (cc.s_maxage >= 0) ttl = cc.s_maxage;
    else if (cc.max_age >= 0) ttl = cc.max_age;
    else ttl = 60; /* default 60s for cacheable responses */

    char *key = cache_build_key(method, uri, vary,
                                 req_hdr_names, req_hdr_values, req_nhdr);
    if (!key) return;

    /* Remove existing entry with same key */
    CacheEntry *existing = cache_find(key);
    if (existing) {
        cache_unlink(existing);
        cache_entry_free(existing);
    }

    /* Create new entry */
    CacheEntry *e = calloc(1, sizeof(CacheEntry));
    if (!e) { free(key); return; }

    e->key = key;
    e->status = status;
    if (body && body_len > 0) {
        e->body = malloc(body_len + 1);
        if (e->body) {
            memcpy(e->body, body, body_len);
            e->body[body_len] = '\0';
        }
        e->body_len = body_len;
    }
    if (etag) e->etag = strdup(etag);
    if (last_modified) e->last_modified = strdup(last_modified);
    if (vary) e->vary = strdup(vary);
    if (content_type) e->content_type = strdup(content_type);
    e->inserted = time(NULL);
    e->ttl = ttl;
    e->last_access = e->inserted;

    g_cache.total_size += body_len;
    g_cache.count++;

    cache_push_front(e);
    cache_evict();

    /* Spill to disk if memory is over limit (63.1.6) */
    if (g_cache.disk_dir && g_cache.total_size > g_cache.max_size) {
        /* Move LRU entry to disk */
        CacheEntry *cold = g_cache.tail;
        if (cold && cold->body) {
            char path[1024];
            /* Use hash of key as filename */
            uint32_t h = 2166136261u;
            for (const char *p = cold->key; *p; p++) {
                h ^= (uint8_t)*p;
                h *= 16777619u;
            }
            snprintf(path, sizeof(path), "%s/%08x.cache",
                     g_cache.disk_dir, h);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                /* Simple format: status\nttl\netag\ntype\nbody */
                dprintf(fd, "%u\n%ld\n%s\n%s\n",
                        cold->status, cold->ttl,
                        cold->etag ? cold->etag : "",
                        cold->content_type ? cold->content_type : "");
                if (cold->body_len > 0) {
                    write(fd, cold->body, cold->body_len);
                }
                close(fd);
                g_cache.disk_used += cold->body_len;
            }
            cache_unlink(cold);
            cache_entry_free(cold);
        }
    }
}

/* ── Purge / invalidation (63.2.1) ───────────────────────────────────── */

/* Purge a specific URI from the cache */
int http_cache_purge(const char *uri)
{
    int purged = 0;
    CacheEntry *e = g_cache.head;
    while (e) {
        CacheEntry *next = e->next;
        /* Key format is "METHOD|URI|vary..." — check if URI matches */
        char *pipe = strchr(e->key, '|');
        if (pipe) {
            pipe++;
            char *pipe2 = strchr(pipe, '|');
            size_t uri_len = pipe2 ? (size_t)(pipe2 - pipe) : strlen(pipe);
            if (strlen(uri) == uri_len &&
                strncmp(pipe, uri, uri_len) == 0) {
                cache_unlink(e);
                cache_entry_free(e);
                purged++;
            }
        }
        e = next;
    }
    return purged;
}

/* Purge by wildcard pattern (e.g. "/api/") */
int http_cache_purge_pattern(const char *pattern)
{
    int purged = 0;
    if (!pattern) return 0;

    /* Simple prefix match for patterns ending in * */
    size_t plen = strlen(pattern);
    int is_wildcard = (plen > 0 && pattern[plen - 1] == '*');
    size_t prefix_len = is_wildcard ? plen - 1 : plen;

    CacheEntry *e = g_cache.head;
    while (e) {
        CacheEntry *next = e->next;
        char *pipe = strchr(e->key, '|');
        if (pipe) {
            pipe++;
            int match;
            if (is_wildcard) {
                match = (strncmp(pipe, pattern, prefix_len) == 0);
            } else {
                char *pipe2 = strchr(pipe, '|');
                size_t uri_len = pipe2 ? (size_t)(pipe2 - pipe) : strlen(pipe);
                match = (strlen(pattern) == uri_len &&
                         strncmp(pipe, pattern, uri_len) == 0);
            }
            if (match) {
                cache_unlink(e);
                cache_entry_free(e);
                purged++;
            }
        }
        e = next;
    }
    return purged;
}

/* ── Micro-caching (63.2.2) ──────────────────────────────────────────── */

/* Store a response with a very short TTL (1-5 seconds).
 * Designed for dynamic content under traffic spikes. */
void http_cache_micro(const char *method, const char *uri,
                       uint32_t status, const char *body, size_t body_len,
                       const char *content_type, int ttl_secs)
{
    if (ttl_secs <= 0) ttl_secs = 1;
    if (ttl_secs > 10) ttl_secs = 10;

    char ttl_str[32];
    snprintf(ttl_str, sizeof(ttl_str), "max-age=%d", ttl_secs);

    http_cache_put(method, uri, status, body, body_len,
                    NULL, NULL, NULL, content_type, ttl_str,
                    NULL, NULL, 0);
}

/* ── Cache status header (63.1.5) ────────────────────────────────────── */

/* Returns "HIT", "MISS", "EXPIRED", "STALE", or "REVALIDATED" */
/* The http_cache_get function already returns the status string. */
/* Callers should add X-Cache: <status> to the response. */

/* ── Stale-if-error (63.2.4) ─────────────────────────────────────────── */

/* Get a stale cache entry when origin returns error.
 * Returns the cached body if available and within stale_if_error window. */
const char *http_cache_stale_if_error(const char *method, const char *uri,
                                       uint32_t *out_status,
                                       size_t *out_body_len,
                                       const char **out_content_type)
{
    /* Simple linear search for any entry matching method|uri prefix */
    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s|%s", method ? method : "GET",
             uri ? uri : "/");
    size_t prefix_len = strlen(prefix);

    for (CacheEntry *e = g_cache.head; e; e = e->next) {
        if (strncmp(e->key, prefix, prefix_len) == 0) {
            time_t age = time(NULL) - e->inserted;
            /* Allow stale up to 5 minutes by default for error fallback */
            if (age < e->ttl + 300) {
                *out_status = e->status;
                *out_body_len = e->body_len;
                *out_content_type = e->content_type;
                return e->body;
            }
        }
    }
    return NULL;
}

/* ── Cache statistics ────────────────────────────────────────────────── */

size_t http_cache_count(void) { return g_cache.count; }
size_t http_cache_size(void) { return g_cache.total_size; }
