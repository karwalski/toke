/* SPDX-License-Identifier: MIT */
/* tk_xalloc.h -- allocation that cannot return NULL.
 *
 * Story 127.439.  Some stdlib constructors and helpers used malloc's result
 * without a check, so an allocation failure crashed on a NULL dereference
 * somewhere downstream (cppcheck nullPointerOutOfMemory).  Where the
 * function has no error channel to report OOM through, it now stops the
 * program at the allocation, the policy tk_runtime.c already follows
 * (`if (!r) abort();`), with a line on stderr saying why.
 */
#ifndef TK_XALLOC_H
#define TK_XALLOC_H

#include <stdio.h>
#include <stdlib.h>

static inline void tk_xalloc_fail(void)
{
    fputs("toke: out of memory\n", stderr);
    abort();
}

/* malloc(n), never NULL; a zero-byte request is served as one byte. */
static inline void *tk_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) tk_xalloc_fail();
    return p;
}

/* calloc(n, sz), never NULL; a zero-element request is served as one. */
static inline void *tk_xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) tk_xalloc_fail();
    return p;
}

#endif /* TK_XALLOC_H */
