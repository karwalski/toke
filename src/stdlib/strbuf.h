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

/* strbuf_dup — a malloc'd, NUL-terminated copy of the contents (len bytes,
 * which may include NULs), leaving b as it is.  NULL on OOM, now or earlier. */
static inline char *strbuf_dup(const TkStrBuf *b)
{
    if (b->oom) return NULL;
    char *s = (char *)malloc(b->len + 1);
    if (!s) return NULL;
    if (b->len) memcpy(s, b->buf, b->len);
    s[b->len] = '\0';
    return s;
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

/* -----------------------------------------------------------------------
 * Context escapers (122.13).  Each appends `s` escaped for ONE output
 * context.  The stdlib's markup/script generators (svg, canvas, chart,
 * dashboard) wrote caller strings into these contexts raw, so a value could
 * close its attribute, element, string literal or comment and inject.  One
 * copy of each rule, here, so the generators cannot drift apart again.
 * NULL appends nothing.  Bytes >= 0x80 pass through unchanged (UTF-8).
 * ----------------------------------------------------------------------- */

/* XML/HTML text AND double- or single-quoted attribute values:
 * & < > " ' become entities. */
static inline void strbuf_append_xml(TkStrBuf *b, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
        case '&':  strbuf_appendn(b, "&amp;", 5);  break;
        case '<':  strbuf_appendn(b, "&lt;", 4);   break;
        case '>':  strbuf_appendn(b, "&gt;", 4);   break;
        case '"':  strbuf_appendn(b, "&quot;", 6); break;
        case '\'': strbuf_appendn(b, "&#39;", 5);  break;
        default:   strbuf_appendn(b, s, 1);        break;
        }
    }
}

/* The body of a JSON string (no surrounding quotes), RFC 8259: '"', '\' and
 * every control byte (U+0000..U+001F, plus DEL) escaped. */
static inline void strbuf_append_json(TkStrBuf *b, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  strbuf_appendn(b, "\\\"", 2); break;
        case '\\': strbuf_appendn(b, "\\\\", 2); break;
        case '\n': strbuf_appendn(b, "\\n", 2);  break;
        case '\r': strbuf_appendn(b, "\\r", 2);  break;
        case '\t': strbuf_appendn(b, "\\t", 2);  break;
        case '\b': strbuf_appendn(b, "\\b", 2);  break;
        case '\f': strbuf_appendn(b, "\\f", 2);  break;
        default:
            if (c < 0x20 || c == 0x7f) strbuf_appendf(b, "\\u%04x", c);
            else strbuf_appendn(b, s, 1);
            break;
        }
    }
}

/* The body of a JavaScript string literal quoted with ' or " (no quotes
 * appended), safe inside an HTML <script> element and inside a JS block
 * comment: both quotes, '\', control bytes, U+2028/U+2029 (line terminators
 * in JS source), '<' and '>' (so "</script>" and "<!--" cannot form) and
 * '/' after '*' (so a comment cannot close) are escaped. */
static inline void strbuf_append_js(TkStrBuf *b, const char *s)
{
    if (!s) return;
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; p++) {
        unsigned char c = *p;
        if (c == '\'' || c == '"' || c == '\\') {
            strbuf_putc(b, '\\'); strbuf_putc(b, (char)c);
        } else if (c == '\n') { strbuf_appendn(b, "\\n", 2);
        } else if (c == '\r') { strbuf_appendn(b, "\\r", 2);
        } else if (c == '\t') { strbuf_appendn(b, "\\t", 2);
        } else if (c < 0x20 || c == 0x7f || c == '<' || c == '>') {
            strbuf_appendf(b, "\\x%02x", c);
        } else if (c == '/' && p > (const unsigned char *)s && p[-1] == '*') {
            strbuf_appendn(b, "\\/", 2);
        } else if (c == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) {
            strbuf_appendf(b, "\\u%04x", p[2] == 0xa8 ? 0x2028 : 0x2029);
            p += 2;
        } else {
            strbuf_putc(b, (char)c);
        }
    }
}

#endif /* TK_STRBUF_H */
