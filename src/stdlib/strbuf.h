/*
 * strbuf.h — the one growable string buffer (story 122.2).
 *
 * WHY THIS EXISTS.  Before 122.2 the stdlib carried a dozen private copies of
 * "append to a buffer": fmt.c's Buf, yaml.c's y_emit, proxy.c's p_emit,
 * toon.c's raw `pos += snprintf(out+pos, cap-pos, ...)`, and buf_* families in
 * html.c, canvas.c, svg.c, csv.c and others.  They had diverged:
 *
 *   - the snprintf-offset family overflowed the heap once pos passed cap
 *     (121.4 yaml, 121.9 proxy); the "fix" clamped instead, so output was
 *     silently TRUNCATED at a fixed cap (proxy even kept the Content-Length
 *     of a body it had dropped);
 *   - toon.c grew by a single doubling, so one value longer than the buffer
 *     still wrote past it;
 *   - fmt.c ignored a failed realloc and memcpy'd past the old buffer.
 *
 * This header replaces them.  Every append either fits or grows; growth
 * checks size_t overflow; an allocation failure is STICKY (b->oom) so a
 * caller can do a run of appends and check once, and after OOM every append
 * is a no-op rather than a write past the end.  The buffer is always
 * NUL-terminated once anything has been appended.
 *
 * WHY HEADER-ONLY, AND WHY HERE (not src/strbuf.[ch] as the row first said).
 * The stdlib directory is shipped on its own: the Homebrew formula installs
 * src/stdlib/ and nothing else from src/, and tkc links a program's stdlib
 * modules by basename from that one directory (stdlib_deps.c).  A strbuf.c
 * would have to be added to the link set of every module that uses it, in
 * stdlib_deps.c, the Makefile and every conform script that compiles a
 * module by hand; a src/strbuf.h outside src/stdlib/ would not be installed
 * at all.  `static inline` in one header in src/stdlib/ needs no link change
 * anywhere, and the compiler (src/fmt.c) includes it as "stdlib/strbuf.h".
 *
 * Usage:
 *     TkStrBuf b = TK_STRBUF_INIT;
 *     strbuf_append(&b, "x=");
 *     strbuf_appendf(&b, "%d", 42);
 *     char *s = strbuf_take(&b);   // malloc'd, caller frees; NULL on OOM
 */
#ifndef TK_STRBUF_H
#define TK_STRBUF_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char   *buf;   /* NULL until the first append; then always NUL-terminated */
    size_t  len;   /* bytes used, excluding the NUL */
    size_t  cap;   /* bytes allocated */
    int     oom;   /* sticky: an allocation failed; appends are now no-ops */
} TkStrBuf;

#define TK_STRBUF_INIT { NULL, 0, 0, 0 }

#if defined(__GNUC__) || defined(__clang__)
#  define TK_STRBUF_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#  define TK_STRBUF_PRINTF(f, a)
#endif

static inline void strbuf_init(TkStrBuf *b)
{
    b->buf = NULL; b->len = 0; b->cap = 0; b->oom = 0;
}

/* strbuf_reserve — make room for `extra` more bytes plus the NUL.
 * Returns 0 on success, -1 (and sets b->oom) on overflow or OOM. */
static inline int strbuf_reserve(TkStrBuf *b, size_t extra)
{
    if (b->oom) return -1;
    if (extra > SIZE_MAX - b->len - 1) { b->oom = 1; return -1; }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) {
        if (nc > SIZE_MAX / 2) { nc = need; break; }
        nc *= 2;
    }
    char *nb = (char *)realloc(b->buf, nc);
    if (!nb) { b->oom = 1; return -1; }
    if (!b->buf) nb[0] = '\0';
    b->buf = nb;
    b->cap = nc;
    return 0;
}

static inline void strbuf_appendn(TkStrBuf *b, const char *s, size_t n)
{
    if (strbuf_reserve(b, n) < 0) return;
    if (n) memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static inline void strbuf_append(TkStrBuf *b, const char *s)
{
    if (s) strbuf_appendn(b, s, strlen(s));
}

static inline void strbuf_putc(TkStrBuf *b, char c)
{
    strbuf_appendn(b, &c, 1);
}

/* strbuf_insertn — insert n bytes at offset pos (pos > len is clamped to len). */
static inline void strbuf_insertn(TkStrBuf *b, size_t pos, const char *s, size_t n)
{
    if (strbuf_reserve(b, n) < 0) return;
    if (pos > b->len) pos = b->len;
    memmove(b->buf + pos + n, b->buf + pos, b->len - pos + 1);   /* +1: the NUL */
    if (n) memcpy(b->buf + pos, s, n);
    b->len += n;
}

static inline void strbuf_vappendf(TkStrBuf *b, const char *fmt, va_list ap)
{
    if (b->oom) return;
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return;                       /* encoding error: append nothing */
    if (strbuf_reserve(b, (size_t)n) < 0) return;
    vsnprintf(b->buf + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

static inline void strbuf_appendf(TkStrBuf *b, const char *fmt, ...)
    TK_STRBUF_PRINTF(2, 3);
static inline void strbuf_appendf(TkStrBuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    strbuf_vappendf(b, fmt, ap);
    va_end(ap);
}

/* strbuf_cstr — the contents so far; "" before the first append.  Valid until
 * the next append.  Never NULL. */
static inline const char *strbuf_cstr(const TkStrBuf *b)
{
    return b->buf ? b->buf : "";
}

static inline void strbuf_free(TkStrBuf *b)
{
    free(b->buf);
    strbuf_init(b);
}

/* strbuf_take — hand the buffer to the caller (malloc'd, NUL-terminated, the
 * caller frees) and reset b.  Returns NULL if any append hit OOM: a partial
 * string is never handed out as if it were whole. */
static inline char *strbuf_take(TkStrBuf *b)
{
    if (b->oom) { strbuf_free(b); return NULL; }
    if (!b->buf && strbuf_reserve(b, 0) < 0) return NULL;
    char *s = b->buf;
    strbuf_init(b);
    return s;
}

#endif /* TK_STRBUF_H */
