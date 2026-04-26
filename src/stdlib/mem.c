/*
 * mem.c — Implementation of the std.mem standard library module.
 *
 * Low-level memory intrinsics: allocation, copy, compare, and
 * single-byte load/store.  All pointer values are passed as i64
 * (cast via intptr_t) to match the toke compiler's uniform ABI.
 *
 * Story: 74.1.3
 */

#include "mem.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* mem.alloc(size:u64):u64 */
int64_t tk_mem_alloc(int64_t size)
{
    void *p = malloc((size_t)size);
    return (int64_t)(intptr_t)p;
}

/* mem.free(ptr:u64):void */
void tk_mem_free(int64_t ptr)
{
    free((void *)(intptr_t)ptr);
}

/* mem.realloc(ptr:u64;size:u64):u64 */
int64_t tk_mem_realloc(int64_t ptr, int64_t size)
{
    void *p = realloc((void *)(intptr_t)ptr, (size_t)size);
    return (int64_t)(intptr_t)p;
}

/* mem.copy(dst:u64;src:u64;n:u64):void */
void tk_mem_copy(int64_t dst, int64_t src, int64_t n)
{
    memcpy((void *)(intptr_t)dst, (const void *)(intptr_t)src, (size_t)n);
}

/* mem.set(dst:u64;val:u64;n:u64):void */
void tk_mem_set(int64_t dst, int64_t val, int64_t n)
{
    memset((void *)(intptr_t)dst, (int)val, (size_t)n);
}

/* mem.cmp(a:u64;b:u64;n:u64):i64 */
int64_t tk_mem_cmp(int64_t a, int64_t b, int64_t n)
{
    return (int64_t)memcmp((const void *)(intptr_t)a,
                           (const void *)(intptr_t)b,
                           (size_t)n);
}

/* mem.load8(ptr:u64;offset:u64):u64 — load one byte, zero-extended */
int64_t tk_mem_load8(int64_t ptr, int64_t offset)
{
    const unsigned char *p = (const unsigned char *)(intptr_t)ptr;
    return (int64_t)p[offset];
}

/* mem.store8(ptr:u64;offset:u64;val:u64):void — store low byte */
void tk_mem_store8(int64_t ptr, int64_t offset, int64_t val)
{
    unsigned char *p = (unsigned char *)(intptr_t)ptr;
    p[offset] = (unsigned char)(val & 0xFF);
}
