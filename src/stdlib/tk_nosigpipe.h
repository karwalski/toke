#ifndef TK_NOSIGPIPE_H
#define TK_NOSIGPIPE_H
/*
 * tk_nosigpipe.h — writing to a socket whose peer has gone must return an
 * error, never raise SIGPIPE (stories 127.144, 127.341, 127.347).
 *
 * The reasoning — per-socket and per-send, never a process-wide
 * signal(SIGPIPE, SIG_IGN) — is the long comment above http_sock_nosigpipe()
 * in http.c.  This header holds the two mechanisms themselves so that a
 * translation unit that does not link http.c (ws.c is linked on its own by
 * test-stdlib-ws and several other stdlib tests) applies the very same guard
 * instead of a copy of it:
 *
 *   TK_SEND_FLAGS     MSG_NOSIGNAL where it exists (Linux; Darwin defines it
 *                     too), else 0.  Pass it to every send() on a socket.
 *   tk_sock_nosigpipe SO_NOSIGPIPE where it exists (Darwin, the BSDs): a
 *                     socket property, so it also covers write(2) calls made
 *                     by code we do not own, such as OpenSSL's socket BIO.
 *   tk_send_all       the whole buffer through send(fd, ..., TK_SEND_FLAGS),
 *                     retrying EINTR and short writes; 0 on success, -1 on
 *                     error with errno set (EPIPE, ECONNRESET, ...).  As in
 *                     http_write_all, ENOTSOCK falls back to write(2): the
 *                     *_handle_fd hooks are also handed pipes and files, and
 *                     a non-socket cannot be the closed-peer case.
 *
 * http.c's HTTP_SEND_FLAGS and http_sock_nosigpipe() are these.
 */
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define TK_SEND_FLAGS MSG_NOSIGNAL
#else
#define TK_SEND_FLAGS 0
#endif

static inline void tk_sock_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

static inline int tk_send_all(int fd, const void *buf, size_t len)
{
    const char *p   = (const char *)buf;
    size_t      off = 0;
    if (fd < 0) { errno = EBADF; return -1; }   /* a closed handle's -1 */
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, TK_SEND_FLAGS);
        if (n < 0 && errno == ENOTSOCK)          /* a pipe or file handed to */
            n = write(fd, p + off, len - off);   /* a *_handle_fd test hook  */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

#endif /* TK_NOSIGPIPE_H */
