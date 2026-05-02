#ifndef TK_STDLIB_SECURE_MEM_H
#define TK_STDLIB_SECURE_MEM_H

/*
 * securemem.h — C interface for the std.securemem standard library module.
 *
 * Provides mlock'd, zero-on-free, TTL-expiring memory buffers for storing
 * short-lived secrets (tokens, passwords, private keys) safely in memory.
 *
 * Type mappings:
 *   str   = const char *  (null-terminated UTF-8)
 *   i32   = int32_t
 *   i64   = int64_t
 *   bool  = int  (0 = false, 1 = true)
 *   ?(str)= TkSecureReadResult  (None represented by is_none=1)
 *
 * Thread safety: all public functions are protected by an internal
 * pthread_mutex_t.  Safe to call from multiple threads.
 *
 * Story: 72.3.1–72.3.4
 */

#include <stdint.h>

/*
 * TkSecureBuf — lightweight handle returned by securemem.alloc().
 *
 * The actual locked memory is managed internally, keyed by id[].
 * id[] is an ASCII decimal string representation of the allocation counter;
 * it is NUL-terminated and fits in 24 bytes.
 */
typedef struct {
    char    id[24];       /* opaque unique identifier (NUL-terminated)  */
    int32_t size;         /* capacity in bytes                           */
    int64_t expires_at;   /* Unix epoch seconds; 0 = no expiry           */
} TkSecureBuf;

/*
 * Optional-string result for securemem.read().
 * is_none=1 means the value is absent (expired, wiped, or not found).
 * When is_none=0, value is a heap-allocated NUL-terminated string; the
 * caller owns it.
 */
typedef struct {
    char *value;   /* heap-allocated copy; caller frees; NULL when is_none */
    int   is_none; /* 1 = None, 0 = Some                                   */
} TkSecureReadResult;

/* -------------------------------------------------------------------------
 * securemem.alloc(size_bytes:i32; ttl_seconds:i32) -> SecureBuf
 *
 * Allocates a locked memory region of size_bytes.  The region is zeroed
 * immediately after allocation and locked with mlock() where available.
 * ttl_seconds=0 means no expiry.  Returns a TkSecureBuf handle.
 * ------------------------------------------------------------------------- */
TkSecureBuf tk_securemem_alloc(int32_t size_bytes, int32_t ttl_seconds);

/* -------------------------------------------------------------------------
 * securemem.write(buf:SecureBuf; data:str) -> bool
 *
 * Copies data into the buffer identified by buf.id.
 * Returns 1 on success, 0 if expired/wiped/data too long/not found.
 * Data longer than buf.size-1 bytes is rejected (not silently truncated).
 * ------------------------------------------------------------------------- */
int tk_securemem_write(TkSecureBuf buf, const char *data);

/* -------------------------------------------------------------------------
 * securemem.read(buf:SecureBuf) -> ?(str)
 *
 * Returns a heap-allocated copy of the buffer contents, or is_none=1 if
 * the buffer has expired, been wiped, or does not exist.
 * Caller owns the returned value (free it when done).
 * ------------------------------------------------------------------------- */
TkSecureReadResult tk_securemem_read(TkSecureBuf buf);

/* -------------------------------------------------------------------------
 * securemem.wipe(buf:SecureBuf) -> bool
 *
 * Zeros the buffer contents and releases the memory region immediately.
 * Returns 1 if found and wiped, 0 if already wiped or not found.
 * ------------------------------------------------------------------------- */
int tk_securemem_wipe(TkSecureBuf buf);

/* -------------------------------------------------------------------------
 * securemem.sweep() -> i32
 *
 * Walks all live allocations, zeros and frees every buffer whose TTL has
 * elapsed, and returns the count freed.
 * ------------------------------------------------------------------------- */
int32_t tk_securemem_sweep(void);

/* -------------------------------------------------------------------------
 * securemem.isavailable() -> bool
 *
 * Returns 1 if mlock() succeeds on a one-page test allocation (i.e. swap
 * protection is functional), 0 otherwise.
 * ------------------------------------------------------------------------- */
int tk_securemem_isavailable(void);

#endif /* TK_STDLIB_SECURE_MEM_H */
