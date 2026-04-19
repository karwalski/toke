/*
 * secure_mem.c — Implementation of the std.secure_mem standard library module.
 *
 * Provides mlock'd, zero-on-free, TTL-expiring memory buffers for secrets.
 *
 * Design:
 *   - Allocations are tracked in a singly-linked list (g_head) protected by
 *     g_mutex (pthread_mutex_t).
 *   - Each node holds a malloc'd payload region that is mlock'd where possible.
 *   - Zeroing uses secure_zero(), which is a volatile-pointer memset loop that
 *     the compiler must not elide, falling back to explicit_bzero() where the
 *     platform provides it.
 *   - Buffer IDs are monotonically increasing decimal strings derived from an
 *     atomic counter (implemented via mutex-protected increment so that C99
 *     atomics are not required).
 *
 * Platform support:
 *   POSIX (Linux, macOS, *BSD): mlock(2) / munlock(2)
 *   Windows:                    VirtualLock() / VirtualUnlock()
 *   Other:                      graceful degradation (no swap protection)
 *
 * Story: 72.3.3
 */

/* Pull in POSIX interfaces (mlock, clock_gettime) on glibc without
 * dragging in everything from _GNU_SOURCE unless already defined. */
#if !defined(_GNU_SOURCE) && !defined(_BSD_SOURCE) && \
    !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "secure_mem.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

/* explicit_bzero is declared in <string.h> on glibc >= 2.25.
 * It is not reliably declared in the macOS SDK headers, so we avoid it there
 * and use the volatile-loop fallback instead (see secure_zero() below). */

/* -------------------------------------------------------------------------
 * Platform-specific includes for mlock / VirtualLock
 * ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
#  define TK_SECURE_MEM_WINDOWS 1
#  include <windows.h>
#  include <memoryapi.h>
#else
#  include <sys/mman.h>   /* mlock, munlock */
#  include <unistd.h>     /* sysconf(_SC_PAGESIZE) */
#endif

/* -------------------------------------------------------------------------
 * secure_zero — guaranteed-not-elided memory zeroing
 *
 * Preference order:
 *   1. explicit_bzero()      (glibc >= 2.25, musl, macOS 10.12+)
 *   2. memset_s()            (C11 Annex K)
 *   3. volatile-pointer loop (fallback, always works in C99)
 * ------------------------------------------------------------------------- */
static void secure_zero(void *ptr, size_t n)
{
    if (!ptr || n == 0) return;

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(ptr, n);
#elif defined(__STDC_LIB_EXT1__)
    memset_s(ptr, n, 0, n);
#else
    /* Volatile-pointer loop: the compiler cannot prove the write is dead
     * because it accesses memory through a volatile-qualified pointer. */
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;
#endif
}

/* -------------------------------------------------------------------------
 * get_epoch_seconds — current Unix time in seconds
 * ------------------------------------------------------------------------- */
static int64_t get_epoch_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec;
}

/* -------------------------------------------------------------------------
 * Internal allocation node
 * ------------------------------------------------------------------------- */
typedef struct SecureNode {
    char    id[24];        /* matches TkSecureBuf.id */
    void   *data;          /* mlock'd heap region     */
    size_t  capacity;      /* size in bytes           */
    int     mlocked;       /* 1 if mlock succeeded    */
    int64_t expires_at;    /* 0 = never               */
    struct SecureNode *next;
} SecureNode;

/* -------------------------------------------------------------------------
 * Global state
 * ------------------------------------------------------------------------- */
static pthread_mutex_t g_mutex   = PTHREAD_MUTEX_INITIALIZER;
static SecureNode     *g_head    = NULL;
static uint64_t        g_counter = 0; /* protected by g_mutex */

/* -------------------------------------------------------------------------
 * Internal helpers (all called with g_mutex held)
 * ------------------------------------------------------------------------- */

/* Find a node by id string. Returns NULL if not found. */
static SecureNode *find_node_locked(const char *id)
{
    SecureNode *n = g_head;
    while (n) {
        if (strcmp(n->id, id) == 0) return n;
        n = n->next;
    }
    return NULL;
}

/* Zero, munlock, and free a node's data.  Does NOT remove it from the list. */
static void destroy_node_data(SecureNode *n)
{
    if (!n->data) return;
    secure_zero(n->data, n->capacity);
#if defined(TK_SECURE_MEM_WINDOWS)
    if (n->mlocked) VirtualUnlock(n->data, n->capacity);
#else
    if (n->mlocked) munlock(n->data, n->capacity);
#endif
    free(n->data);
    n->data = NULL;
    n->mlocked = 0;
}

/* Unlink and free a node that is known to be in the list. */
static void remove_node_locked(SecureNode *n)
{
    destroy_node_data(n);
    if (g_head == n) {
        g_head = n->next;
    } else {
        SecureNode *prev = g_head;
        while (prev && prev->next != n) prev = prev->next;
        if (prev) prev->next = n->next;
    }
    free(n);
}

/* -------------------------------------------------------------------------
 * tk_secure_mem_alloc
 * ------------------------------------------------------------------------- */
TkSecureBuf tk_secure_mem_alloc(int32_t size_bytes, int32_t ttl_seconds)
{
    TkSecureBuf result;
    memset(&result, 0, sizeof(result));

    if (size_bytes <= 0) {
        /* Return a zeroed handle; caller can detect via id[0]=='\0'. */
        return result;
    }

    SecureNode *node = (SecureNode *)malloc(sizeof(SecureNode));
    if (!node) return result;
    memset(node, 0, sizeof(SecureNode));

    /* Allocate the data region. */
    node->data = malloc((size_t)size_bytes);
    if (!node->data) {
        free(node);
        return result;
    }
    node->capacity = (size_t)size_bytes;

    /* Zero immediately. */
    secure_zero(node->data, node->capacity);

    /* Attempt to lock the page into RAM. */
#if defined(TK_SECURE_MEM_WINDOWS)
    node->mlocked = VirtualLock(node->data, node->capacity) ? 1 : 0;
    if (!node->mlocked) {
        fprintf(stderr, "[secure_mem] WARNING: VirtualLock failed; "
                        "buffer will not be swap-protected\n");
    }
#else
    node->mlocked = (mlock(node->data, node->capacity) == 0) ? 1 : 0;
    if (!node->mlocked) {
        fprintf(stderr, "[secure_mem] WARNING: mlock failed; "
                        "buffer will not be swap-protected\n");
    }
#endif

    pthread_mutex_lock(&g_mutex);

    /* Assign a unique ID. */
    g_counter++;
    snprintf(node->id, sizeof(node->id), "%llu",
             (unsigned long long)g_counter);

    /* Set expiry. */
    if (ttl_seconds > 0) {
        node->expires_at = get_epoch_seconds() + (int64_t)ttl_seconds;
    } else {
        node->expires_at = 0;
    }

    /* Prepend to list. */
    node->next = g_head;
    g_head = node;

    /* Fill result handle. */
    memcpy(result.id, node->id, sizeof(result.id));
    result.size       = size_bytes;
    result.expires_at = node->expires_at;

    pthread_mutex_unlock(&g_mutex);
    return result;
}

/* -------------------------------------------------------------------------
 * tk_secure_mem_write
 * ------------------------------------------------------------------------- */
int tk_secure_mem_write(TkSecureBuf buf, const char *data)
{
    if (!data || buf.id[0] == '\0') return 0;

    pthread_mutex_lock(&g_mutex);

    SecureNode *n = find_node_locked(buf.id);
    if (!n || !n->data) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* Check expiry. */
    if (n->expires_at != 0 && get_epoch_seconds() >= n->expires_at) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* Bounds check: data must fit with a NUL terminator. */
    size_t data_len = strlen(data);
    if (data_len >= n->capacity) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* Zero first, then copy — prevents partial old data lingering. */
    secure_zero(n->data, n->capacity);
    memcpy(n->data, data, data_len);
    /* NUL terminator already present from the zero pass. */

    pthread_mutex_unlock(&g_mutex);
    return 1;
}

/* -------------------------------------------------------------------------
 * tk_secure_mem_read
 * ------------------------------------------------------------------------- */
TkSecureReadResult tk_secure_mem_read(TkSecureBuf buf)
{
    TkSecureReadResult result = {NULL, 1};

    if (buf.id[0] == '\0') return result;

    pthread_mutex_lock(&g_mutex);

    SecureNode *n = find_node_locked(buf.id);
    if (!n || !n->data) {
        pthread_mutex_unlock(&g_mutex);
        return result;
    }

    /* Check expiry. */
    if (n->expires_at != 0 && get_epoch_seconds() >= n->expires_at) {
        pthread_mutex_unlock(&g_mutex);
        return result;
    }

    /* Copy the data out. */
    size_t len = strnlen((const char *)n->data, n->capacity);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        pthread_mutex_unlock(&g_mutex);
        return result;
    }
    memcpy(copy, n->data, len);
    copy[len] = '\0';

    pthread_mutex_unlock(&g_mutex);

    result.value   = copy;
    result.is_none = 0;
    return result;
}

/* -------------------------------------------------------------------------
 * tk_secure_mem_wipe
 * ------------------------------------------------------------------------- */
int tk_secure_mem_wipe(TkSecureBuf buf)
{
    if (buf.id[0] == '\0') return 0;

    pthread_mutex_lock(&g_mutex);

    SecureNode *n = find_node_locked(buf.id);
    if (!n || !n->data) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    remove_node_locked(n);

    pthread_mutex_unlock(&g_mutex);
    return 1;
}

/* -------------------------------------------------------------------------
 * tk_secure_mem_sweep
 * ------------------------------------------------------------------------- */
int32_t tk_secure_mem_sweep(void)
{
    int32_t   count = 0;
    int64_t   now   = get_epoch_seconds();
    SecureNode *n, *next;

    pthread_mutex_lock(&g_mutex);

    n = g_head;
    while (n) {
        next = n->next;
        if (n->expires_at != 0 && now >= n->expires_at) {
            remove_node_locked(n);
            count++;
        }
        n = next;
    }

    pthread_mutex_unlock(&g_mutex);
    return count;
}

/* -------------------------------------------------------------------------
 * tk_secure_mem_is_available
 * ------------------------------------------------------------------------- */
int tk_secure_mem_is_available(void)
{
#if defined(TK_SECURE_MEM_WINDOWS)
    /* Allocate one page, attempt VirtualLock, then free. */
    SIZE_T page = 4096;
    void *p = VirtualAlloc(NULL, page, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (!p) return 0;
    int ok = VirtualLock(p, page) ? 1 : 0;
    if (ok) VirtualUnlock(p, page);
    VirtualFree(p, 0, MEM_RELEASE);
    return ok;
#elif defined(_SC_PAGESIZE)
    /* POSIX: allocate one page, attempt mlock, then free. */
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    void *p = malloc((size_t)page);
    if (!p) return 0;
    int ok = (mlock(p, (size_t)page) == 0) ? 1 : 0;
    if (ok) munlock(p, (size_t)page);
    free(p);
    return ok;
#else
    return 0;
#endif
}
