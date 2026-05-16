/*
 * securemem_glue.c — i64-ABI wrappers for std.securemem module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.securemem.
 */

#include "securemem.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * TkSecureBuf is a 40-byte struct (id[24] + i32 size + i64 expires_at).
 * At ABI level, toke passes a pointer to a heap-allocated struct as i64.
 * The glue allocates TkSecureBuf on the heap and returns pointer-as-i64.
 */

/* securemem.alloc(size, ttl) — allocate a secure buffer */
int64_t tk_securemem_alloc_w(int64_t size, int64_t ttl) {
    TkSecureBuf buf = tk_securemem_alloc((int32_t)size, (int32_t)ttl);
    TkSecureBuf *heap = (TkSecureBuf *)malloc(sizeof(TkSecureBuf));
    if (!heap) return 0;
    *heap = buf;
    return (int64_t)(intptr_t)heap;
}

/* securemem.write(buf, data) — write secret into buffer */
int64_t tk_securemem_write_w(int64_t buf_ptr, int64_t data) {
    if (!buf_ptr || !data) return 0;
    TkSecureBuf *bp = (TkSecureBuf *)(intptr_t)buf_ptr;
    return (int64_t)tk_securemem_write(*bp, (const char *)(intptr_t)data);
}

/* securemem.read(buf) — read secret from buffer, return string or 0 */
int64_t tk_securemem_read_w(int64_t buf_ptr) {
    if (!buf_ptr) return 0;
    TkSecureBuf *bp = (TkSecureBuf *)(intptr_t)buf_ptr;
    TkSecureReadResult r = tk_securemem_read(*bp);
    if (r.is_none) return 0;
    return (int64_t)(intptr_t)r.value;
}

/* securemem.wipe(buf) — zero and free the buffer */
int64_t tk_securemem_wipe_w(int64_t buf_ptr) {
    if (!buf_ptr) return 0;
    TkSecureBuf *bp = (TkSecureBuf *)(intptr_t)buf_ptr;
    return (int64_t)tk_securemem_wipe(*bp);
}

/* securemem.sweep() — free all expired buffers, return count freed */
int64_t tk_securemem_sweep_w(int64_t dummy) {
    (void)dummy;
    return (int64_t)tk_securemem_sweep();
}
