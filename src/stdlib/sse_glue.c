/*
 * sse_glue.c — i64-ABI wrappers for std.sse (Story 136.32).
 *
 * std.sse is one of the six modules src/stdlib_deps.c registered with no glue
 * file at all: sse.c has implemented the W3C EventSource formatting since
 * story 15.1.2 and stdlib/sse.tki declares all four entry points, but nothing
 * mapped `sse.emit` to a symbol, so both examples on docs/stdlib/sse.md died
 * at link.
 *
 * ABI shapes
 * ----------
 * `$ssectx{id:u64; open:bool}` and `$sseevent{id;event;data;retry}` are
 * ordinary toke structs, so each crosses as a pointer to a block of i64
 * slots in field order — the same convention df_shape_impl and ml_glue.c use.
 * The C side wants a TkSseCtx*, which is opaque and carries the open/closed
 * flag, so a small process-local registry maps ctx id -> TkSseCtx*, created
 * on first use. Ids are connection identifiers, not pointers, so a toke
 * program that rebuilds `$ssectx{id:1;open:true}` twice addresses one stream.
 *
 * Formatted blocks are written to the file descriptor named by ctx.id, which
 * is what "SSE connection identifier" means for a server holding the socket;
 * id 0 means stdout, so the documented single-process examples emit where a
 * reader can see them.
 */
#include "sse.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 114.53/127.67: a wrapper reports failure through this flag as well as
 * through its return value. Defined in tk_runtime.c.
 *
 * Caveat found while verifying this file (story 136.32, filed separately):
 * the compiler currently lowers a call returning `void!$err` as a VOID call,
 * discards the result and substitutes a literal 0 before the arm test
 * (`%t = add i64 0, 0 ; void call result` / `icmp ne %t, 0`), so `mt
 * sse.emit(...)` takes the $err arm unconditionally no matter what this code
 * returns or sets. The event blocks themselves are written correctly. The
 * flag is maintained here so the arms come out right the moment the lowering
 * does. */
/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

/* Slot offsets in the toke struct blocks (field order from stdlib/sse.tki). */
#define SSECTX_ID    0
#define SSECTX_OPEN  1
#define SSEEV_ID     0
#define SSEEV_EVENT  1
#define SSEEV_DATA   2
#define SSEEV_RETRY  3

#define TK_SSE_MAX 64

typedef struct { int64_t id; TkSseCtx *ctx; } TkSseSlot;
static TkSseSlot g_sse[TK_SSE_MAX];
static int       g_sse_count = 0;

static TkSseCtx *ctx_for(int64_t id) {
    for (int i = 0; i < g_sse_count; i++)
        if (g_sse[i].id == id) return g_sse[i].ctx;
    if (g_sse_count >= TK_SSE_MAX) return NULL;
    TkSseCtx *c = sse_new();
    if (!c) return NULL;
    g_sse[g_sse_count].id  = id;
    g_sse[g_sse_count].ctx = c;
    g_sse_count++;
    return c;
}

/* Write a formatted block to the connection fd, then release it. Returns 0 on
 * success and -1 on failure, which is the void!$sseerr convention used by
 * tk_ws_send_w. */
static int64_t sse_write(int64_t id, const char *block) {
    if (!block) { tk_current_error = 1; return -1; }
    int fd = (int)id;
    if (fd <= 0) fd = 1;              /* 0 = stdout for standalone programs */
    size_t len = strlen(block);
    ssize_t n = write(fd, block, len);
    free((void *)block);
    int ok = (n == (ssize_t)len);
    tk_current_error = ok ? 0 : 1;
    return ok ? 0 : -1;
}

int64_t tk_sse_emit_w(int64_t ctx_h, int64_t ev_h) {
    if (!ctx_h || !ev_h) { tk_current_error = 1; return -1; }
    int64_t *c = (int64_t *)(intptr_t)ctx_h;
    int64_t *e = (int64_t *)(intptr_t)ev_h;
    if (!c[SSECTX_OPEN]) { tk_current_error = 1; return -1; }
    TkSseCtx *ctx = ctx_for(c[SSECTX_ID]);
    if (!ctx) { tk_current_error = 1; return -1; }

    TkSseEvent ev;
    ev.id    = (const char *)(intptr_t)e[SSEEV_ID];
    ev.event = (const char *)(intptr_t)e[SSEEV_EVENT];
    ev.data  = (const char *)(intptr_t)e[SSEEV_DATA];
    /* retry is u64 in toke and "omit" is -1 in C; 0 means "not set" here,
     * which is how the documented `retry:0` example reads. */
    ev.retry = e[SSEEV_RETRY] > 0 ? (int)e[SSEEV_RETRY] : -1;

    return sse_write(c[SSECTX_ID], sse_emit(ctx, ev));
}

int64_t tk_sse_emitdata_w(int64_t ctx_h, int64_t data) {
    if (!ctx_h || !data) { tk_current_error = 1; return -1; }
    int64_t *c = (int64_t *)(intptr_t)ctx_h;
    if (!c[SSECTX_OPEN]) { tk_current_error = 1; return -1; }
    TkSseCtx *ctx = ctx_for(c[SSECTX_ID]);
    if (!ctx) { tk_current_error = 1; return -1; }
    return sse_write(c[SSECTX_ID], sse_emitdata(ctx, (const char *)(intptr_t)data));
}

/*
 * sse.keepalive(ctx; intervalms) : void — emit ONE ": keepalive" comment.
 *
 * sse_keepalive() formats a single comment; a wrapper cannot own a timer, and
 * a caller that wants a cadence already has the loop. intervalms is therefore
 * the cadence the CALLER is keeping, not something this function schedules;
 * docs/stdlib/sse.md says so.
 */
int64_t tk_sse_keepalive_w(int64_t ctx_h, int64_t intervalms) {
    (void)intervalms;
    if (!ctx_h) return 0;
    int64_t *c = (int64_t *)(intptr_t)ctx_h;
    if (!c[SSECTX_OPEN]) return 0;
    TkSseCtx *ctx = ctx_for(c[SSECTX_ID]);
    if (!ctx) return 0;
    sse_write(c[SSECTX_ID], sse_keepalive(ctx));
    return 0;
}

int64_t tk_sse_close_w(int64_t ctx_h) {
    if (!ctx_h) return 0;
    int64_t *c = (int64_t *)(intptr_t)ctx_h;
    TkSseCtx *ctx = ctx_for(c[SSECTX_ID]);
    if (ctx) sse_close(ctx);          /* marks closed; always returns NULL */
    c[SSECTX_OPEN] = 0;
    return 0;
}
