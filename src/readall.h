#ifndef TK_READALL_H
#define TK_READALL_H
/*
 * readall.h — read a whole FILE* into a NUL-terminated heap buffer.
 *
 * Story 135.3j (2).  Every source/companion/.tkir reader in the driver used
 * the fseek(SEEK_END)/ftell/rewind idiom to size its buffer.  On a stream
 * that cannot seek — a FIFO, a pipe, /dev/stdin — ftell returns -1, so the
 * reader did malloc((size_t)-1 + 1) == malloc(0) and then
 * fread(buf, 1, (size_t)-1, f): a heap overflow driven by the source file.
 * rewind() cannot report failure at all.
 *
 * tk_read_stream() never seeks: it reads in chunks into a buffer that grows
 * by doubling, so it works on regular files and on streams alike.  It is
 * header-only (static inline) so the driver, companion and .tkir readers
 * share one copy without a new translation unit in either Makefile.
 *
 * Returns a malloc'd buffer (caller frees) holding the bytes read followed
 * by a NUL, and stores the byte count in *out_len.  Returns NULL on a read
 * error, on allocation failure, or when the stream holds more than max_len
 * bytes (then *too_big, if given, is set to 1).
 */
#include <stdio.h>
#include <stdlib.h>

/* Largest source the driver accepts: main.c sizes its token array as
 * (int)(len + 16) * sizeof(Token) (20 bytes), which stays below INT_MAX
 * with room to spare at 64 MiB.  No real toke source is within 100x of it. */
#define TK_SOURCE_MAX ((size_t)64 << 20)

/* First allocation: large enough for a typical source file in one read. */
#define TK_READALL_CHUNK 65536

static inline char *tk_read_stream(FILE *f, size_t max_len, size_t *out_len,
                                   int *too_big)
{
    size_t cap = TK_READALL_CHUNK, len = 0;
    char *buf = (char *)malloc(cap + 1);
    if (out_len) *out_len = 0;
    if (too_big) *too_big = 0;
    if (!buf) return NULL;
    for (;;) {
        if (len == cap) {
            if (cap > max_len) { free(buf); if (too_big) *too_big = 1; return NULL; }
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap + 1);
            if (!nb) { free(buf); return NULL; }
            buf = nb; cap = ncap;
        }
        size_t want = cap - len;
        size_t n = fread(buf + len, 1, want, f);
        len += n;
        /* fread returns short only at end-of-file or on an error (it blocks
         * on a pipe until it has `want` bytes or the writer closes), and
         * reading again after either is undefined for the stream position. */
        if (n < want) break;
    }
    if (ferror(f)) { free(buf); return NULL; }
    if (len > max_len) { free(buf); if (too_big) *too_big = 1; return NULL; }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

#endif /* TK_READALL_H */
