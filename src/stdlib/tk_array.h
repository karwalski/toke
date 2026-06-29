/*
 * tk_array.h — toke array backing-block layout + helpers (ADR-0006 / story 114.18).
 *
 * Block layout (all i64):
 *
 *     [ rc | cap | len | data[0] | data[1] | ... | data[len-1] ]
 *                        ^ handle
 *
 * The handle (the i64 value that crosses the toke ABI) points at &data[0], so:
 *
 *     handle[-1] == len      handle[-2] == cap      handle[-3] == rc
 *
 * len stays at handle[-1] exactly as in the pre-114.18 single-word layout, so
 * every existing length read (`ptr[-1]`) and element read (`ptr[i]`) is
 * unchanged — only the *construction* (allocation + header init) sites move to
 * the 3-word header, and the mutation ops (append/set/pop) gain the rc/cap
 * copy-on-write fast path.
 *
 * Copy-on-write (monotonic refcount, ADR-0006 D2): `tk_arr_retain` bumps rc on
 * every array-typed handle duplication (emitted by codegen). A mutation may
 * write in place only when rc == 1 (the array is unaliased); otherwise it
 * copies. rc is never decremented and blocks are never freed (toke's current
 * leak-forever model is preserved), so an over-count can only cause an
 * unnecessary copy — never a use-after-free.
 */
#ifndef TK_ARRAY_H
#define TK_ARRAY_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TK_ARR_HDR 3 /* header words: rc, cap, len */

/* Length of an array handle (NULL-safe). Reads handle[-1]. */
static inline int64_t tk_arr_len(int64_t h) {
    return h ? ((int64_t *)(intptr_t)h)[-1] : 0;
}
/* Capacity / refcount — only valid on new-header arrays (post-114.18). */
static inline int64_t tk_arr_cap(int64_t h) { return ((int64_t *)(intptr_t)h)[-2]; }
static inline int64_t tk_arr_rc(int64_t h)  { return ((int64_t *)(intptr_t)h)[-3]; }
static inline void tk_arr_setlen(int64_t h, int64_t n) { ((int64_t *)(intptr_t)h)[-1] = n; }

/*
 * tk_arr_alloc — allocate a backing block with capacity `cap` (raised to `len`
 * if smaller), length `len`, refcount 1. Returns the data handle (or 0 on OOM);
 * the `len` data slots are uninitialised.
 */
static inline int64_t tk_arr_alloc(int64_t cap, int64_t len) {
    if (cap < len) cap = len;
    if (cap < 0)   cap = 0;
    int64_t *b = (int64_t *)malloc((size_t)(cap + TK_ARR_HDR) * sizeof(int64_t));
    if (!b) return 0;
    b[0] = 1;    /* rc  */
    b[1] = cap;  /* cap */
    b[2] = len;  /* len */
    return (int64_t)(intptr_t)(b + TK_ARR_HDR);
}

#endif /* TK_ARRAY_H */
