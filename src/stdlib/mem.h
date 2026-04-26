#ifndef TK_STDLIB_MEM_H
#define TK_STDLIB_MEM_H

/*
 * mem.h — C interface for the std.mem standard library module.
 *
 * Provides low-level memory intrinsics: allocation, copy, compare,
 * and single-byte load/store.  All pointer values are passed as i64
 * (cast via intptr_t) to match the toke compiler's uniform ABI.
 *
 * Story: 74.1.2
 */

#include <stdint.h>

/* mem.alloc(size:u64):u64 — allocate size bytes, return pointer as i64 */
int64_t  tk_mem_alloc(int64_t size);

/* mem.free(ptr:u64):void — free a previously allocated block */
void     tk_mem_free(int64_t ptr);

/* mem.realloc(ptr:u64;size:u64):u64 — resize allocation */
int64_t  tk_mem_realloc(int64_t ptr, int64_t size);

/* mem.copy(dst:u64;src:u64;n:u64):void — copy n bytes from src to dst */
void     tk_mem_copy(int64_t dst, int64_t src, int64_t n);

/* mem.set(dst:u64;val:u64;n:u64):void — fill n bytes at dst with val */
void     tk_mem_set(int64_t dst, int64_t val, int64_t n);

/* mem.cmp(a:u64;b:u64;n:u64):i64 — compare n bytes, return <0/0/>0 */
int64_t  tk_mem_cmp(int64_t a, int64_t b, int64_t n);

/* mem.load8(ptr:u64;offset:u64):u64 — load one byte, zero-extended */
int64_t  tk_mem_load8(int64_t ptr, int64_t offset);

/* mem.store8(ptr:u64;offset:u64;val:u64):void — store low byte */
void     tk_mem_store8(int64_t ptr, int64_t offset, int64_t val);

#endif /* TK_STDLIB_MEM_H */
