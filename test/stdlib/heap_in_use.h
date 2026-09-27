/*
 * heap_in_use.h — the process heap's live byte count, for the retention
 * tests (test_http_keepalive_leak.c, test_http_route_param_leak.c).
 *
 * 122.14 item 3: both tests read the SYSTEM allocator's statistics —
 * malloc_zone_statistics() on macOS, mallinfo2() on glibc.  Under
 * AddressSanitizer the allocator is ASan's own: its zone reports its shadow
 * and quarantine bookkeeping, and both tests measured ~1500 bytes retained
 * per request on a tree that retains none (and 0.0 without ASan), so they
 * were excluded from `make test-asan-stdlib`.  Under ASan this header asks
 * ASan instead: __sanitizer_get_current_allocated_bytes() is the bytes the
 * program has malloc()ed and not yet free()d — a freed chunk sitting in
 * quarantine is not counted — which is exactly what the tests mean.
 *
 * HEAP_STATS_SOURCE names which counter a run used, so a log says what it
 * measured.  HAVE_HEAP_STATS is 0 where no counter exists (the tests SKIP).
 */
#ifndef TK_TEST_HEAP_IN_USE_H
#define TK_TEST_HEAP_IN_USE_H

#include <stddef.h>
#include <string.h>

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define TK_TEST_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(TK_TEST_ASAN)
#  define TK_TEST_ASAN 1
#endif

#if defined(TK_TEST_ASAN)
#  include <sanitizer/allocator_interface.h>
#  define HAVE_HEAP_STATS 1
#  define HEAP_STATS_SOURCE "__sanitizer_get_current_allocated_bytes (ASan)"
static size_t heap_in_use(void)
{
    return __sanitizer_get_current_allocated_bytes();
}
#elif defined(__APPLE__)
#  include <malloc/malloc.h>
#  define HAVE_HEAP_STATS 1
#  define HEAP_STATS_SOURCE "malloc_zone_statistics"
static size_t heap_in_use(void)
{
    malloc_statistics_t st;
    memset(&st, 0, sizeof st);
    malloc_zone_statistics(NULL, &st);  /* NULL = all zones */
    return (size_t)st.size_in_use;
}
#elif defined(__linux__) && defined(__GLIBC__)
#  include <malloc.h>
#  define HAVE_HEAP_STATS 1
#  define HEAP_STATS_SOURCE "mallinfo2"
static size_t heap_in_use(void)
{
    struct mallinfo2 mi = mallinfo2();
    return (size_t)mi.uordblks;
}
#else
#  define HAVE_HEAP_STATS 0
#  define HEAP_STATS_SOURCE "none"
static size_t heap_in_use(void) { return 0; }
#endif

#endif /* TK_TEST_HEAP_IN_USE_H */
