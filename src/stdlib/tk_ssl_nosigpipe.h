/*
 * tk_ssl_nosigpipe.h — TLS records written with send(MSG_NOSIGNAL), so an
 * https client whose server has gone gets an error, not SIGPIPE (127.346).
 *
 * THE DEFECT.  127.341 made the plain-HTTP client SIGPIPE-proof: SO_NOSIGPIPE
 * on the socket (Darwin/BSD) and MSG_NOSIGNAL on every send() (Linux).  The
 * https client hands its socket to OpenSSL with SSL_set_fd, and OpenSSL's
 * socket BIO writes with write(2).  On Darwin SO_NOSIGPIPE is a socket
 * property and covers that write; on Linux there is no SO_NOSIGPIPE and
 * MSG_NOSIGNAL is a send() flag OpenSSL never passes, so a server that closes
 * mid-request still killed the toke program with status 141.
 *
 * THE CHOICE.  Per socket, not per process.  signal(SIGPIPE, SIG_IGN) in the
 * runtime was the other option and is rejected for the reason 127.144 gives
 * in http.c: it changes what every write in every toke program does, and an
 * ignored disposition is inherited across fork() and execve().  Instead the
 * SSL object gets OpenSSL's ordinary socket BIO for READING (reads cannot
 * raise SIGPIPE) and this small BIO for WRITING, whose only job is
 * send(fd, ..., MSG_NOSIGNAL).  Nothing outside the connection changes.
 *
 * It is used wherever MSG_NOSIGNAL exists — Darwin defines it too — so the
 * BIO is exercised by every https test on macOS as well, not compiled only on
 * the one platform this host cannot run.  Where MSG_NOSIGNAL does not exist
 * it falls back to SSL_set_fd, exactly as before.
 *
 * The BIO does not own the descriptor (BIO_NOCLOSE semantics, like the
 * socket BIO SSL_set_fd makes): the caller closes fd as it always has.
 * Kernel TLS offload is not offered on the write side (BIO_CTRL_GET_KTLS_SEND
 * answers 0); the client never enabled it.
 *
 * Header-only and static, like tk_ssl_host.h, so no link line changes.
 */
#ifndef TK_SSL_NOSIGPIPE_H
#define TK_SSL_NOSIGPIPE_H

#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>

#ifdef MSG_NOSIGNAL

static int tk_nsp_bio_write(BIO *b, const char *buf, int len)
{
    int     fd = (int)(intptr_t)BIO_get_data(b);
    ssize_t n;
    BIO_clear_retry_flags(b);
    if (len <= 0) return 0;
    do {
        n = send(fd, buf, (size_t)len, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        BIO_set_retry_write(b);
    return (int)n;
}

static long tk_nsp_bio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    int fd = (int)(intptr_t)BIO_get_data(b);
    (void)num;
    switch (cmd) {
    case BIO_CTRL_FLUSH:                 /* send() does not buffer */
        return 1;
    case BIO_C_GET_FD:
        if (ptr) *(int *)ptr = fd;
        return fd;
    case BIO_CTRL_GET_CLOSE:
        return BIO_NOCLOSE;
    default:                             /* as the socket BIO: unknown -> 0 */
        return 0;
    }
}

static int tk_nsp_bio_create(BIO *b)
{
    BIO_set_data(b, (void *)(intptr_t)-1);
    BIO_set_init(b, 1);
    return 1;
}

static int tk_nsp_bio_destroy(BIO *b)
{
    (void)b;                             /* never closes fd */
    return 1;
}

static BIO_METHOD     *tk_nsp_method;
static pthread_once_t  tk_nsp_once = PTHREAD_ONCE_INIT;

static void tk_nsp_method_init(void)
{
    BIO_METHOD *m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK
                                 | BIO_TYPE_DESCRIPTOR,
                                 "toke socket write (MSG_NOSIGNAL)");
    if (!m) return;
    if (BIO_meth_set_write(m, tk_nsp_bio_write) != 1
        || BIO_meth_set_ctrl(m, tk_nsp_bio_ctrl) != 1
        || BIO_meth_set_create(m, tk_nsp_bio_create) != 1
        || BIO_meth_set_destroy(m, tk_nsp_bio_destroy) != 1) {
        BIO_meth_free(m);
        return;
    }
    tk_nsp_method = m;                   /* lives for the process */
}

#endif /* MSG_NOSIGNAL */

/*
 * tk_ssl_set_fd_nosigpipe — SSL_set_fd, except that writes go through
 * send(MSG_NOSIGNAL).  Returns 1 on success and 0 on failure, as SSL_set_fd.
 */
static inline int tk_ssl_set_fd_nosigpipe(SSL *ssl, int fd)
{
#ifdef MSG_NOSIGNAL
    BIO *rbio, *wbio;
    pthread_once(&tk_nsp_once, tk_nsp_method_init);
    if (!tk_nsp_method) return 0;
    rbio = BIO_new_socket(fd, BIO_NOCLOSE);
    if (!rbio) return 0;
    wbio = BIO_new(tk_nsp_method);
    if (!wbio) { BIO_free(rbio); return 0; }
    BIO_set_data(wbio, (void *)(intptr_t)fd);
    SSL_set_bio(ssl, rbio, wbio);        /* ssl owns both from here */
    return 1;
#else
    return SSL_set_fd(ssl, fd);
#endif
}

#endif /* TK_SSL_NOSIGPIPE_H */
