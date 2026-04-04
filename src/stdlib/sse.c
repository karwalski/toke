/*
 * sse.c — Implementation of the std.sse standard library module.
 *
 * Implements the Server-Sent Events wire format as defined by the W3C
 * EventSource specification.  Events are returned as heap-allocated
 * null-terminated strings; callers are responsible for freeing them.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own the returned pointers.
 *
 * Story: 15.1.2
 */

#include "sse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal context
 * ----------------------------------------------------------------------- */

struct TkSseCtx {
    int closed;
};

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/*
 * count_lines — count the number of '\n'-delimited lines in s.
 * A string with no '\n' is 1 line.  An empty string is 1 line.
 */
static int count_lines(const char *s)
{
    int n = 1;
    for (; *s; s++) {
        if (*s == '\n') n++;
    }
    return n;
}

/*
 * data_block_size — calculate the number of bytes needed to render all
 * data: lines for the given data string (not including the trailing blank
 * line that terminates the event).
 *
 * Each logical line becomes "data: <line>\n".
 */
static size_t data_block_size(const char *data)
{
    /* "data: " = 6 chars, "\n" = 1 char per line */
    const size_t prefix_len = 6; /* strlen("data: ") */
    size_t total = 0;
    const char *p = data;
    while (1) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        total += prefix_len + line_len + 1; /* "data: " + line + "\n" */
        if (!nl) break;
        p = nl + 1;
    }
    return total;
}

/*
 * append_data_lines — write all "data: <line>\n" entries for data into buf,
 * advancing *pos past the written bytes.
 */
static void append_data_lines(char *buf, size_t *pos, const char *data)
{
    const char *p = data;
    while (1) {
        const char *nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        memcpy(buf + *pos, "data: ", 6);
        *pos += 6;
        memcpy(buf + *pos, p, line_len);
        *pos += line_len;
        buf[(*pos)++] = '\n';
        if (!nl) break;
        p = nl + 1;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

TkSseCtx *sse_new(void)
{
    TkSseCtx *ctx = (TkSseCtx *)malloc(sizeof(TkSseCtx));
    if (!ctx) return NULL;
    ctx->closed = 0;
    return ctx;
}

void sse_free(TkSseCtx *ctx)
{
    if (ctx) free(ctx);
}

const char *sse_emit(TkSseCtx *ctx, TkSseEvent ev)
{
    if (!ctx || ctx->closed) return NULL;

    /* Calculate required buffer size. */
    size_t needed = 0;

    /* event: <name>\n */
    if (ev.event) {
        needed += 7 + strlen(ev.event) + 1; /* "event: " + name + "\n" */
    }

    /* data: <line>\n  (one or more lines) */
    if (ev.data) {
        needed += data_block_size(ev.data);
    } else {
        /* Emit an empty data line so the event is valid SSE. */
        needed += 7; /* "data: \n" */
    }

    /* id: <id>\n */
    if (ev.id) {
        needed += 4 + strlen(ev.id) + 1; /* "id: " + id + "\n" */
    }

    /* retry: <ms>\n  — at most "retry: 2147483647\n" = 20 chars */
    if (ev.retry >= 0) {
        needed += 22;
    }

    /* trailing blank line */
    needed += 1;

    /* NUL terminator */
    needed += 1;

    char *buf = (char *)malloc(needed);
    if (!buf) return NULL;

    size_t pos = 0;

    /* event: */
    if (ev.event) {
        memcpy(buf + pos, "event: ", 7);
        pos += 7;
        size_t n = strlen(ev.event);
        memcpy(buf + pos, ev.event, n);
        pos += n;
        buf[pos++] = '\n';
    }

    /* data: */
    if (ev.data) {
        append_data_lines(buf, &pos, ev.data);
    } else {
        memcpy(buf + pos, "data: \n", 7);
        pos += 7;
    }

    /* id: */
    if (ev.id) {
        memcpy(buf + pos, "id: ", 4);
        pos += 4;
        size_t n = strlen(ev.id);
        memcpy(buf + pos, ev.id, n);
        pos += n;
        buf[pos++] = '\n';
    }

    /* retry: */
    if (ev.retry >= 0) {
        int written = snprintf(buf + pos, needed - pos, "retry: %d\n", ev.retry);
        if (written > 0) pos += (size_t)written;
    }

    /* blank line terminating the event */
    buf[pos++] = '\n';
    buf[pos]   = '\0';

    return buf;
}

const char *sse_emitdata(TkSseCtx *ctx, const char *data)
{
    TkSseEvent ev;
    ev.event = NULL;
    ev.data  = data;
    ev.id    = NULL;
    ev.retry = -1;
    return sse_emit(ctx, ev);
}

const char *sse_keepalive(TkSseCtx *ctx)
{
    if (!ctx || ctx->closed) return NULL;
    const char *msg = ": keepalive\n\n";
    size_t len = strlen(msg);
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, msg, len + 1);
    return buf;
}

const char *sse_close(TkSseCtx *ctx)
{
    if (ctx) ctx->closed = 1;
    return NULL;
}

int sse_is_closed(TkSseCtx *ctx)
{
    if (!ctx) return 1;
    return ctx->closed;
}
