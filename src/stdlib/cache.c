/* SPDX-License-Identifier: MIT */
/*
 * cache.c — HTTP response cache for std.http.
 *
 * Epic 63: in-memory LRU cache, Cache-Control parsing, conditional requests,
 * Vary-aware keying, disk-backed tier, purge/invalidation, micro-caching,
 * stale-while-revalidate, stale-if-error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>

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

static void cache_spill_victim(const CacheEntry *e);

/* Evict LRU entries until under limits.  Each victim is offered to the disk
 * tier first (63.1.6, 127.496): with a disk_dir set it is spilled, then freed.
 * keep (may be NULL) is never evicted: http_cache_get passes the entry it just
 * read back from disk, whose pointers it is about to hand out. */
static void cache_evict_keep(const CacheEntry *keep)
{
    while (g_cache.tail && g_cache.tail != keep &&
           (g_cache.count > g_cache.max_entries ||
            g_cache.total_size > g_cache.max_size)) {
        CacheEntry *victim = g_cache.tail;
        cache_unlink(victim);
        cache_spill_victim(victim);
        cache_entry_free(victim);
    }
}

static void cache_evict(void) { cache_evict_keep(NULL); }

/* ── Cache lookup ────────────────────────────────────────────────────── */

static CacheEntry *cache_find(const char *key)
{
    for (CacheEntry *e = g_cache.head; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

/* ── Disk spill (63.1.6, 127.445) ────────────────────────────────────── */

/* Write all of buf to fd: retry on EINTR, continue after a short write.
 * Returns 0 when every byte was written, -1 otherwise (errno set; a write
 * that returns 0 counts as ENOSPC). */
static int cache_write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) { errno = ENOSPC; return -1; }
        buf += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* On-disk format (127.496): a text header, then the raw bytes.
 *
 *     TKC1\n<status>\n<ttl>\n<inserted>\n<klen> <elen> <clen> <blen>\n
 *     <key><etag><content_type><body>
 *
 * Each length is a decimal byte count; -1 for an etag or content type means
 * absent (NULL).  The key is stored so a read-back can tell its own entry from
 * another key whose FNV-1a filename collides, and inserted so the TTL keeps
 * counting from the original store.  A file whose size is not exactly the
 * header plus the four lengths is not an entry (a truncated or foreign file). */
#define CACHE_DISK_MAGIC "TKC1\n"

static long cache_slen(const char *s) { return s ? (long)strlen(s) : -1; }

/* Write one entry to path in the format above.  Returns 0 and counts body_len
 * in disk_used only when the whole file was written and closed.  On any
 * failure (open, a write, close) the partial file is removed, nothing is
 * counted and -1 is returned with errno from the failing call.  The cache has
 * no error channel (http_cache_put is void): a spill that fails leaves the
 * entry evicted, which is a later MISS, never a truncated body (127.445). */
static int cache_spill_file(const char *path, const CacheEntry *e)
{
    const char *key = e->key ? e->key : "";
    long klen = cache_slen(key);
    long elen = cache_slen(e->etag), clen = cache_slen(e->content_type);
    int hlen = snprintf(NULL, 0, CACHE_DISK_MAGIC "%u\n%ld\n%lld\n%ld %ld %ld %zu\n",
                        e->status, e->ttl, (long long)e->inserted,
                        klen, elen, clen, e->body_len);
    if (hlen < 0) return -1;
    char *hdr = malloc((size_t)hlen + 1);
    if (!hdr) return -1;
    snprintf(hdr, (size_t)hlen + 1, CACHE_DISK_MAGIC "%u\n%ld\n%lld\n%ld %ld %ld %zu\n",
             e->status, e->ttl, (long long)e->inserted,
             klen, elen, clen, e->body_len);

    /* Remove whatever is at path (a symlink itself, never its target), then
     * create with O_EXCL|O_NOFOLLOW: a file or symlink planted there in
     * between is not followed or truncated; the spill fails instead (a later
     * MISS).  0600: cached bodies are not left readable by other users. */
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { int er = errno; free(hdr); errno = er; return -1; }

    int rc = cache_write_all(fd, hdr, (size_t)hlen);
    int er = errno;
    free(hdr);
    const char *parts[4] = { key, e->etag, e->content_type, e->body };
    size_t lens[4] = { (size_t)klen, elen > 0 ? (size_t)elen : 0,
                       clen > 0 ? (size_t)clen : 0, e->body_len };
    for (int i = 0; i < 4 && rc == 0; i++) {
        if (lens[i] == 0) continue;
        rc = cache_write_all(fd, parts[i], lens[i]);
        er = errno;
    }
    if (close(fd) != 0 && rc == 0) { rc = -1; er = errno; }
    if (rc != 0) {
        unlink(path);
        errno = er;
        return -1;
    }
    g_cache.disk_used += e->body_len;
    return 0;
}

/* The spill file for key: disk_dir/<FNV-1a of key>.cache */
static void cache_disk_path(const char *key, char *path, size_t cap)
{
    uint32_t h = 2166136261u;
    for (const char *p = key; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    snprintf(path, cap, "%s/%08x.cache", g_cache.disk_dir, h);
}

/* Free an entry that is not in the LRU list and was never counted. */
static void cache_entry_discard(CacheEntry *e)
{
    if (!e) return;
    free(e->key); free(e->body); free(e->etag);
    free(e->last_modified); free(e->vary); free(e->content_type);
    free(e);
}

/* Parse one decimal field ending in term; -1 is allowed when neg is set. */
static int cache_parse_num(const char **pp, const char *end, char term,
                           int neg, long long *out)
{
    const char *p = *pp;
    int minus = 0;
    if (neg && p < end && *p == '-') { minus = 1; p++; }
    if (p >= end || *p < '0' || *p > '9') return -1;
    long long v = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        if (v > (LLONG_MAX - 9) / 10) return -1;
        v = v * 10 + (*p - '0');
        p++;
    }
    if (p >= end || *p != term) return -1;
    if (minus && v != 1) return -1;
    *out = minus ? -v : v;
    *pp = p + 1;
    return 0;
}

static char *cache_dup_n(const char *p, long long n)
{
    if (n < 0) return NULL;
    char *s = malloc((size_t)n + 1);
    if (!s) return NULL;
    memcpy(s, p, (size_t)n);
    s[n] = '\0';
    return s;
}

/* Read the spill file at path into a new, unlinked, uncounted entry.
 * Returns NULL when the file is absent, unreadable or not exactly one entry
 * in the format above. */
static CacheEntry *cache_disk_load(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    size_t sz = (size_t)st.st_size;
    char *buf = malloc(sz);
    size_t got = 0;
    while (buf && got < sz) {
        ssize_t n = read(fd, buf + got, sz - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != sz) { free(buf); return NULL; }

    const char *p = buf, *end = buf + sz;
    long long status, ttl, ins, kl, el, cl, bl;
    size_t ml = sizeof(CACHE_DISK_MAGIC) - 1;
    CacheEntry *e = NULL;
    if (sz < ml || memcmp(p, CACHE_DISK_MAGIC, ml) != 0) goto out;
    p += ml;
    if (cache_parse_num(&p, end, '\n', 0, &status) || status > UINT32_MAX ||
        cache_parse_num(&p, end, '\n', 0, &ttl) ||
        cache_parse_num(&p, end, '\n', 0, &ins) ||
        cache_parse_num(&p, end, ' ', 0, &kl) ||
        cache_parse_num(&p, end, ' ', 1, &el) ||
        cache_parse_num(&p, end, ' ', 1, &cl) ||
        cache_parse_num(&p, end, '\n', 0, &bl))
        goto out;
    /* Each length is checked against what is left before it is consumed, so
     * a crafted header cannot wrap the sum and point p outside buf. */
    size_t rest = (size_t)(end - p);
    long long lens[4] = { kl, el > 0 ? el : 0, cl > 0 ? cl : 0, bl };
    for (int i = 0; i < 4; i++) {
        if ((unsigned long long)lens[i] > rest) goto out;
        rest -= (size_t)lens[i];
    }
    if (rest != 0) goto out;

    e = calloc(1, sizeof(CacheEntry));
    if (!e) goto out;
    e->status = (uint32_t)status;
    e->ttl = (long)ttl;
    e->inserted = (time_t)ins;
    e->last_access = time(NULL);
    /* last_modified and vary are not in the spill format: a read-back entry
     * has them NULL.  Nothing reads either field after a put (Vary is already
     * part of the key), so the read-back is lossy only in those two fields. */
    e->key = cache_dup_n(p, kl);              p += kl;
    e->etag = cache_dup_n(p, el);             p += el > 0 ? el : 0;
    e->content_type = cache_dup_n(p, cl);     p += cl > 0 ? cl : 0;
    e->body_len = (size_t)bl;
    if (bl > 0) e->body = cache_dup_n(p, bl);
    if (!e->key || (el >= 0 && !e->etag) || (cl >= 0 && !e->content_type) ||
        (bl > 0 && !e->body)) {
        cache_entry_discard(e);
        e = NULL;
    }
out:
    free(buf);
    return e;
}

/* Remove the spill file at path and uncount the body it holds (a file this
 * process did not write was never counted: disk_used does not go below 0). */
static void cache_disk_remove(const char *path, size_t body_len)
{
    if (unlink(path) == 0)
        g_cache.disk_used = g_cache.disk_used > body_len
                          ? g_cache.disk_used - body_len : 0;
}

/* Remove key's spill file, if the file at its path holds key. */
static void cache_disk_forget(const char *key)
{
    if (!g_cache.disk_dir) return;
    char path[1024];
    cache_disk_path(key, path, sizeof(path));
    CacheEntry *d = cache_disk_load(path);
    if (d && strcmp(d->key, key) == 0) cache_disk_remove(path, d->body_len);
    cache_entry_discard(d);
}

/* Spill an eviction victim to disk_dir (63.1.6).  Whatever file sits at its
 * path (an older copy, or another key that collides) is replaced.  A spill
 * that would take disk_used past disk_max_size is not made: the entry is
 * dropped, a later MISS. */
static void cache_spill_victim(const CacheEntry *e)
{
    if (!g_cache.disk_dir || !e->key) return;
    if (!e->body && e->body_len > 0) return;
    char path[1024];
    cache_disk_path(e->key, path, sizeof(path));
    CacheEntry *old = cache_disk_load(path);
    if (old) cache_disk_remove(path, old->body_len);
    else unlink(path);
    cache_entry_discard(old);
    if (e->body_len > g_cache.disk_max_size ||
        g_cache.disk_used > g_cache.disk_max_size - e->body_len)
        return;
    (void)cache_spill_file(path, e);
}

/* Move e, just loaded from the spill file at path, to the front of the memory
 * tier: its file is removed and uncounted, and the eviction it causes never
 * takes e itself (its pointers are about to be handed out). */
static CacheEntry *cache_disk_adopt(CacheEntry *e, const char *path)
{
    cache_disk_remove(path, e->body_len);
    g_cache.total_size += e->body_len;
    g_cache.count++;
    cache_push_front(e);
    cache_evict_keep(e);
    return e;
}

/* Read key back from disk_dir: the entry moves to the front of the memory
 * tier and is returned; NULL when there is no spill file holding key. */
static CacheEntry *cache_disk_promote(const char *key)
{
    if (!g_cache.disk_dir) return NULL;
    char path[1024];
    cache_disk_path(key, path, sizeof(path));
    CacheEntry *e = cache_disk_load(path);
    if (!e) return NULL;
    if (strcmp(e->key, key) != 0) { cache_entry_discard(e); return NULL; }
    return cache_disk_adopt(e, path);
}

/* Is de_name a spill file's name, <8 hex digits>.cache? */
static int cache_disk_name(const char *de_name)
{
    for (int i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)de_name[i])) return 0;
    return strcmp(de_name + 8, ".cache") == 0;
}

/* Does key "METHOD|URI|vary..." name uri (wildcard: URI starts with the
 * first plen bytes of uri)?  http_cache_purge and http_cache_purge_pattern
 * apply it to both tiers. */
static int cache_key_names(const char *key, const char *uri, int wildcard,
                           size_t plen)
{
    const char *pipe = strchr(key, '|');
    if (!pipe) return 0;
    pipe++;
    if (wildcard) return strncmp(pipe, uri, plen) == 0;
    const char *pipe2 = strchr(pipe, '|');
    size_t uri_len = pipe2 ? (size_t)(pipe2 - pipe) : strlen(pipe);
    return strlen(uri) == uri_len && strncmp(pipe, uri, uri_len) == 0;
}

/* Purge matching spill files from disk_dir; returns how many. */
static int cache_disk_purge(const char *uri, int wildcard, size_t plen)
{
    if (!g_cache.disk_dir) return 0;
    DIR *d = opendir(g_cache.disk_dir);
    if (!d) return 0;
    int purged = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!cache_disk_name(de->d_name)) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", g_cache.disk_dir, de->d_name);
        CacheEntry *e = cache_disk_load(path);
        if (e && cache_key_names(e->key, uri, wildcard, plen)) {
            cache_disk_remove(path, e->body_len);
            purged++;
        }
        cache_entry_discard(e);
    }
    closedir(d);
    return purged;
}

/* ── Public API ──────────────────────────────────────────────────────── */

/* Initialise the cache (call once) */
void http_cache_init(size_t max_entries, size_t max_size_bytes)
{
    g_cache.max_entries = max_entries > 0 ? max_entries : 10000;
    g_cache.max_size = max_size_bytes > 0 ? max_size_bytes : (64 * 1024 * 1024);
}

/* Set disk cache directory (63.1.6).  Entries evicted from memory are
 * spilled here while disk_used + body length stays within max_size_bytes
 * (0: nothing is spilled), and http_cache_get reads them back (127.496). */
void http_cache_set_disk(const char *dir, size_t max_size_bytes)
{
    free(g_cache.disk_dir);
    g_cache.disk_dir = strdup(dir);
    g_cache.disk_max_size = max_size_bytes;
    g_cache.disk_used = 0;
    /* Create directory if needed */
    mkdir(dir, 0755);
}

/* Check cache for a response. Returns cache status string.
 * The out_body, out_etag and out_content_type pointers point into the cache
 * and are valid only until the next http_cache_get, http_cache_put, purge or
 * http_cache_stale_if_error call: since 127.496 a get that reads an entry back from disk can evict
 * (and free) the entries an earlier get returned.  Copy what must outlive
 * that. */
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
    /* Not in memory: read it back from the disk tier (63.1.6, 127.496) */
    if (!e) e = cache_disk_promote(key);
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

    /* Remove existing entry with same key, in memory and on disk */
    CacheEntry *existing = cache_find(key);
    if (existing) {
        cache_unlink(existing);
        cache_entry_free(existing);
    }
    cache_disk_forget(key);

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
    /* Over a limit: the LRU entries are spilled to disk_dir (when one is set)
     * and leave memory (63.1.6, 127.496). */
    cache_evict();
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
        if (cache_key_names(e->key, uri, 0, 0)) {
            cache_unlink(e);
            cache_entry_free(e);
            purged++;
        }
        e = next;
    }
    return purged + cache_disk_purge(uri, 0, 0);
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
        if (cache_key_names(e->key, pattern, is_wildcard, prefix_len)) {
            cache_unlink(e);
            cache_entry_free(e);
            purged++;
        }
        e = next;
    }
    return purged + cache_disk_purge(pattern, is_wildcard, prefix_len);
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

/* Is e a stale-if-error fallback for "METHOD|URI" (prefix, plen) at now:
 * its key is "METHOD|URI" itself or a Vary variant of it ("METHOD|URI|..."),
 * and it is within its TTL plus 5 minutes?  127.514: the byte after the
 * prefix must end the key or be the Vary separator, so /a never falls back
 * to /ab's (or /a/b's) body. */
static int cache_stale_match(const CacheEntry *e, const char *prefix,
                             size_t plen, time_t now)
{
    /* Allow stale up to 5 minutes by default for error fallback */
    return strncmp(e->key, prefix, plen) == 0 &&
           (e->key[plen] == '\0' || e->key[plen] == '|') &&
           now - e->inserted < e->ttl + 300;
}

/* The stale-if-error fallback on disk (127.511): scan disk_dir as
 * cache_disk_purge does, through cache_disk_load's bounds-checked read-back,
 * and adopt the matching spill file with the latest insertion time (the
 * freshest copy; directory order is arbitrary) into memory.  NULL when no
 * spill file matches. */
static CacheEntry *cache_disk_stale(const char *prefix, size_t plen, time_t now)
{
    if (!g_cache.disk_dir) return NULL;
    DIR *d = opendir(g_cache.disk_dir);
    if (!d) return NULL;
    CacheEntry *best = NULL;
    char best_path[1024] = "";
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!cache_disk_name(de->d_name)) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", g_cache.disk_dir, de->d_name);
        CacheEntry *e = cache_disk_load(path);
        if (e && cache_stale_match(e, prefix, plen, now) &&
            (!best || e->inserted > best->inserted)) {
            cache_entry_discard(best);
            best = e;
            snprintf(best_path, sizeof(best_path), "%s", path);
        } else {
            cache_entry_discard(e);
        }
    }
    closedir(d);
    return best ? cache_disk_adopt(best, best_path) : NULL;
}

/* Get a stale cache entry when origin returns error.
 * Returns the cached body if available and within stale_if_error window.
 * The memory tier is searched first, then the spill files in disk_dir
 * (127.511); an entry found on disk moves to the front of memory.  The
 * returned pointers are valid only until the next http_cache_get,
 * http_cache_put, purge or http_cache_stale_if_error call (that move can
 * evict). */
const char *http_cache_stale_if_error(const char *method, const char *uri,
                                       uint32_t *out_status,
                                       size_t *out_body_len,
                                       const char **out_content_type)
{
    /* Linear search for the entry keyed "METHOD|URI" or a Vary variant of
     * it (127.514: not any key that merely starts with those bytes) */
    char prefix[1024];
    snprintf(prefix, sizeof(prefix), "%s|%s", method ? method : "GET",
             uri ? uri : "/");
    size_t prefix_len = strlen(prefix);
    time_t now = time(NULL);

    CacheEntry *e = g_cache.head;
    while (e && !cache_stale_match(e, prefix, prefix_len, now)) e = e->next;
    if (!e) e = cache_disk_stale(prefix, prefix_len, now);
    if (!e) return NULL;
    *out_status = e->status;
    *out_body_len = e->body_len;
    *out_content_type = e->content_type;
    return e->body;
}

/* ── Cache statistics ────────────────────────────────────────────────── */

size_t http_cache_count(void) { return g_cache.count; }
size_t http_cache_size(void) { return g_cache.total_size; }
