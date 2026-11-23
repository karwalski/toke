/* SPDX-License-Identifier: MIT */
/*
 * securemem_glue.c — i64-ABI wrappers for std.securemem module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.securemem.
 */

#include "securemem.h"
#include "tk_runtime.h"   /* 138.45: the handle registry */
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
    /* 138.45: registered, so a value alloc did not hand out is refused. */
    if (!tk_handle_adopt(TK_HK_SECUREBUF, heap)) { tk_securemem_wipe(buf); free(heap); return 0; }
    return (int64_t)(intptr_t)heap;
}

/* securemem.write(buf, data) — write secret into buffer */
/* 138.45: a securebuf is looked up in the runtime's handle registry before it
 * is read; any value securemem.alloc did not hand out is answered as the null
 * buffer is (write/wipe false, read none).  It used to be read as a
 * TkSecureBuf and the program died with SIGSEGV (securemem.wipe(5)). */
static int64_t sbuf_in(int64_t v) { return (int64_t)(intptr_t)tk_handle_addr(TK_HK_SECUREBUF, v); }

int64_t tk_securemem_write_w(int64_t buf_ptr, int64_t data) {
    buf_ptr = sbuf_in(buf_ptr);
    if (!buf_ptr || !data) return 0;
    TkSecureBuf *bp = (TkSecureBuf *)(intptr_t)buf_ptr;
    return (int64_t)tk_securemem_write(*bp, (const char *)(intptr_t)data);
}

/* securemem.read(buf) — read secret from buffer, return string or 0 */
int64_t tk_securemem_read_w(int64_t buf_ptr) {
    buf_ptr = sbuf_in(buf_ptr);
    if (!buf_ptr) return 0;
    TkSecureBuf *bp = (TkSecureBuf *)(intptr_t)buf_ptr;
    TkSecureReadResult r = tk_securemem_read(*bp);
    if (r.is_none) return 0;
    return (int64_t)(intptr_t)r.value;
}

/* securemem.wipe(buf) — zero and free the buffer */
int64_t tk_securemem_wipe_w(int64_t buf_ptr) {
    buf_ptr = sbuf_in(buf_ptr);
    if (!buf_ptr) return 0;
    TkSecureBuf *bp = (TkSecureBuf *)(intptr_t)buf_ptr;
    return (int64_t)tk_securemem_wipe(*bp);
}

/* securemem.sweep() — free all expired buffers, return count freed */
int64_t tk_securemem_sweep_w(void) {
    return (int64_t)tk_securemem_sweep();
}

/* securemem.isavailable() — 1 if mlock() works on a probe page, else 0.
 * 136.5: the core has existed since 72.3.3 as tk_securemem_isavailable(); only
 * this wrapper was missing, so the .tki exported a name with no symbol behind
 * it. Same shape as keychain.isavailable in 136.3. */
int64_t tk_securemem_isavailable_w(void) {
    return (int64_t)tk_securemem_isavailable();
}
