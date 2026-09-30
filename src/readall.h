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
#if defined(__unix__) || defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#define TK_READALL_POSIX 1
#endif

/* Largest source the driver accepts: main.c sizes its token array as
 * (int)(len + 16) * sizeof(Token) (20 bytes), which stays below INT_MAX
 * with room to spare at 64 MiB.  No real toke source is within 100x of it. */
#define TK_SOURCE_MAX ((size_t)64 << 20)

/* First allocation: large enough for a typical source file in one read. */
#define TK_READALL_CHUNK 65536

#ifdef TK_READALL_POSIX
/*
 * 127.254: a FIFO or pipe is read with NON-BLOCKING read(2) calls, waiting
 * between them in select(2) with a 100 ms bound -- never in a blocking read.
 *
 * Measured on Darwin 25 under load (16 concurrent loops, load average ~25):
 * 9 FIFO reads in 4,000 had consumed every byte the writer sent, the
 * writer had closed (no process held the write end), and the reader slept in
 * read(2) for good -- `tkc --verify-companion comp.fifo` sat there 36 minutes
 * in a conform run.  The kernel had recorded the EOF: a second writer merely
 * opening and closing the FIFO woke the reader, which then saw end-of-file.
 * The wakeup for the first writer's close was lost.  A plain open/read probe
 * with no tkc code in it hangs the same way, so the defect is not a second
 * open or a mis-sized read in tkc.  A non-blocking read re-asks the kernel
 * each time, so a lost wakeup now costs at most one 100 ms select timeout.
 * poll(2) cannot stand in for select here: on Darwin it never reports
 * POLLHUP/POLLIN at a FIFO's end-of-file (measured), select does.
 *
 * Called only on a stream nothing has been read from yet (every caller has
 * just fopen'd it), so bypassing stdio's buffer loses nothing.  Returns 1 and
 * sets out, out_len and too_big as tk_read_stream does, or 0 when fd is not a
 * FIFO/pipe (the caller then uses stdio).
 */
static inline int tk_read_fifo_fd(int fd, size_t max_len, char **out,
                                  size_t *out_len, int *too_big)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode)) return 0;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return 0;
    size_t cap = TK_READALL_CHUNK, len = 0;
    char *buf = (char *)malloc(cap + 1);
    int ok = buf != NULL;
    while (ok) {
        if (len == cap) {
            if (cap > max_len) { if (too_big) *too_big = 1; ok = 0; break; }
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(buf, ncap + 1);
            if (!nb) { ok = 0; break; }
            buf = nb; cap = ncap;
        }
        ssize_t n = read(fd, buf + len, cap - len);
        if (n > 0) { len += (size_t)n; continue; }
        if (n == 0) break;                          /* end of file */
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) { ok = 0; break; }
        if (fd >= FD_SETSIZE) { ok = 0; break; }    /* FD_SET would overflow */
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;                        /* bound, not a poll rate */
        (void)select(fd + 1, &rd, NULL, NULL, &tv); /* then read again */
    }
    (void)fcntl(fd, F_SETFL, fl);
    if (ok && len > max_len) { if (too_big) *too_big = 1; ok = 0; }
    if (!ok) { free(buf); *out = NULL; return 1; }
    buf[len] = '\0';
    *out = buf;
    if (out_len) *out_len = len;
    return 1;
}
#endif

static inline char *tk_read_stream(FILE *f, size_t max_len, size_t *out_len,
                                   int *too_big)
{
    size_t cap = TK_READALL_CHUNK, len = 0;
    char *buf = (char *)malloc(cap + 1);
    if (out_len) *out_len = 0;
    if (too_big) *too_big = 0;
    if (!buf) return NULL;
#ifdef TK_READALL_POSIX
    {
        char *fb = NULL;
        if (tk_read_fifo_fd(fileno(f), max_len, &fb, out_len, too_big)) {
            free(buf);
            return fb;
        }
    }
#endif
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
