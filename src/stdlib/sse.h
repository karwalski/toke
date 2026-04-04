#ifndef TK_STDLIB_SSE_H
#define TK_STDLIB_SSE_H

/*
 * sse.h — C interface for the std.sse standard library module.
 *
 * Type mappings:
 *   Str  = const char *  (null-terminated UTF-8)
 *   bool = int  (0 = false, 1 = true)
 *
 * SSE (Server-Sent Events) is a text-based streaming format defined by
 * the W3C EventSource specification.  This implementation buffers events
 * as heap-allocated strings; the caller owns every returned pointer.
 *
 * No external dependencies beyond libc.
 *
 * Story: 15.1.2
 */

#include <stdint.h>

/* Opaque SSE context.  Tracks open/closed state. */
typedef struct TkSseCtx TkSseCtx;

/*
 * TkSseEvent — describes a single SSE event block.
 *
 * Fields set to NULL (or retry == -1) are omitted from the output.
 */
typedef struct {
    const char *event;  /* event name, NULL = omitted                        */
    const char *data;   /* data field; embedded '\n' → multiple data: lines  */
    const char *id;     /* id field, NULL = omitted                          */
    int         retry;  /* retry interval in ms; -1 = omitted                */
} TkSseEvent;

/* sse_new — allocate and initialise a new SSE context.
 * Returns NULL on allocation failure. */
TkSseCtx   *sse_new(void);

/* sse_free — release all resources owned by ctx. */
void        sse_free(TkSseCtx *ctx);

/* sse_emit — format ev as an SSE event block.
 * Returns a heap-allocated string the caller must free, or NULL on error.
 * Has no effect (returns NULL) if the context is closed. */
const char *sse_emit(TkSseCtx *ctx, TkSseEvent ev);

/* sse_emitdata — shorthand: emit a data-only event.
 * Equivalent to sse_emit with only the data field set. */
const char *sse_emitdata(TkSseCtx *ctx, const char *data);

/* sse_keepalive — emit an SSE comment keepalive (": keepalive\n\n").
 * Returns a heap-allocated string the caller must free, or NULL on error. */
const char *sse_keepalive(TkSseCtx *ctx);

/* sse_close — mark the context as closed.
 * Always returns NULL (signals end-of-stream to the caller). */
const char *sse_close(TkSseCtx *ctx);

/* sse_is_closed — returns 1 if the context has been closed, 0 otherwise. */
int         sse_is_closed(TkSseCtx *ctx);

#endif /* TK_STDLIB_SSE_H */
