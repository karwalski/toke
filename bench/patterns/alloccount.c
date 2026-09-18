/* alloccount.c — count malloc-family calls and requested bytes in any process.
 *
 * Build:  make -C bench/patterns            (-> liballoccount.dylib)
 * Use:    DYLD_INSERT_LIBRARIES=bench/patterns/liballoccount.dylib ./prog
 *
 * Interposes malloc/calloc/realloc/free via the dyld __interpose section
 * (works on any non-system binary; SIP only blocks Apple-signed binaries).
 * On process exit a destructor prints one line to stderr:
 *   ALLOC calls=<n> bytes=<b> malloc=<m> calloc=<c> realloc=<r> free=<f>
 * calls = malloc+calloc+realloc; bytes = sum of requested sizes (realloc is
 * counted at its new size, so bytes is an upper bound on live growth).
 * Programs that exit via _exit() skip destructors and print nothing.
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee)                                   \
    __attribute__((used)) static struct {                                         \
        const void *replacement;                                                  \
        const void *replacee;                                                     \
    } _interpose_##_replacee __attribute__((section("__DATA,__interpose"))) = {   \
        (const void *)(unsigned long)&_replacement,                               \
        (const void *)(unsigned long)&_replacee};

static _Atomic unsigned long long n_malloc, n_calloc, n_realloc, n_free, n_bytes;

static void *ac_malloc(size_t sz) {
    atomic_fetch_add_explicit(&n_malloc, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&n_bytes, sz, memory_order_relaxed);
    return malloc(sz);
}
static void *ac_calloc(size_t n, size_t sz) {
    atomic_fetch_add_explicit(&n_calloc, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&n_bytes, n * sz, memory_order_relaxed);
    return calloc(n, sz);
}
static void *ac_realloc(void *p, size_t sz) {
    atomic_fetch_add_explicit(&n_realloc, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&n_bytes, sz, memory_order_relaxed);
    return realloc(p, sz);
}
static void ac_free(void *p) {
    atomic_fetch_add_explicit(&n_free, 1, memory_order_relaxed);
    free(p);
}

DYLD_INTERPOSE(ac_malloc, malloc)
DYLD_INTERPOSE(ac_calloc, calloc)
DYLD_INTERPOSE(ac_realloc, realloc)
DYLD_INTERPOSE(ac_free, free)

__attribute__((destructor)) static void ac_report(void) {
    char buf[256];
    unsigned long long m = n_malloc, c = n_calloc, r = n_realloc, f = n_free, b = n_bytes;
    int len = snprintf(buf, sizeof buf,
                       "ALLOC calls=%llu bytes=%llu malloc=%llu calloc=%llu realloc=%llu free=%llu\n",
                       m + c + r, b, m, c, r, f);
    if (len > 0) (void)write(2, buf, (size_t)len);
}
