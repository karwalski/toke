/* SPDX-License-Identifier: MIT */
/*
 * tk_bind.h — bind a TCP address without shadowing a live listener
 * (stories 135.32, 135.33).
 *
 * THE PROBLEM.  On Darwin (and the BSDs) a socket that sets SO_REUSEADDR may
 * bind 127.0.0.1:P beside a live *:P listener, or *:P beside a live
 * 127.0.0.1:P, and the more specific socket then takes that address's
 * traffic.  Measured on Darwin 25.6 for every combination: without
 * SO_REUSEADDR each of those binds is EADDRINUSE; with it both succeed.  So
 * a second toke server on a busy port could start "successfully" and
 * silently take loopback traffic from the first, and a "is this port free?"
 * probe that set SO_REUSEADDR answered yes for a port a server was using.
 * Linux refuses a bind that overlaps a listening socket whether or not
 * SO_REUSEADDR is set.
 *
 * SO_REUSEADDR is still needed for the ordinary restart: a port whose old
 * connections sit in TIME_WAIT is EADDRINUSE without it.  The two cases are
 * told apart by asking the port:
 *
 *   1. bind WITHOUT SO_REUSEADDR.  Success -> done.
 *   2. EADDRINUSE: connect to the address (127.0.0.1 for a wildcard bind).
 *      Something accepts -> a listener is live: fail, EADDRINUSE.
 *   3. Nothing accepts (refused, or no answer within one second):
 *      TIME_WAIT or a bound non-listening socket -> retry WITH SO_REUSEADDR,
 *      which is exactly what every bind did before.
 *
 * Linux is unchanged by construction: step 1 fails in a superset of the
 * cases the old bind failed in; where the probe answers, a listener
 * overlaps and Linux's SO_REUSEADDR bind would have failed too; everywhere
 * else step 3 is the old bind.
 *
 * KNOWN GAP.  A wildcard bind probes loopback only, so a listener on one
 * specific non-loopback address (say 192.168.1.5:P) is not detected: the
 * wildcard server starts, and on Darwin that address's traffic still goes
 * to the first server while every other address's goes to the new one.
 * The user-facing statement of this limit is in docs/stdlib/net.md and
 * docs/stdlib/router.md.
 *
 * Used by: http.c (serve and its forms, C-API http_serve), router.c
 * (router.serve), net_glue.c (net.listen), net.c (net.portavailable),
 * tls.c (tls.listen, 135.34).  (server_ops.c's proc_config_test used it
 * until story 121.71 archived that uncalled file.)  Header-only
 * and static, like tk_exitfail.h, so every stdlib translation unit that
 * includes it links without a new object in any link line.
 */
#ifndef TK_BIND_H
#define TK_BIND_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#if defined(__GNUC__) || defined(__clang__)
#  define TK_BIND_UNUSED __attribute__((unused))
#else
#  define TK_BIND_UNUSED
#endif

/* 1 when something accepts a connection on addr (loopback for a wildcard). */
static TK_BIND_UNUSED int tk_port_has_listener(const struct sockaddr_in *addr)
{
    struct sockaddr_in to = *addr;
    if (to.sin_addr.s_addr == htonl(INADDR_ANY))
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int answered = 0;
    if (connect(s, (const struct sockaddr *)&to, sizeof to) == 0) {
        answered = 1;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd = { s, POLLOUT, 0 };
        if (poll(&pfd, 1, 1000) == 1) {
            int err = 0; socklen_t el = sizeof err;
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                answered = 1;
        }
    }
    close(s);
    return answered;
}

/* A bound (not yet listening) AF_INET stream fd, or -1 with errno set. */
static TK_BIND_UNUSED int tk_bind_exclusive(const struct sockaddr_in *addr)
{
    int opt = 1;
    for (int pass = 0; pass < 2; pass++) {
        int srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) return -1;
        if (pass == 1)
            setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(srv, (const struct sockaddr *)addr, sizeof(*addr)) == 0)
            return srv;
        int e = errno;
        close(srv);
        if (pass == 1 || e != EADDRINUSE || addr->sin_port == 0 ||
            tk_port_has_listener(addr)) {
            errno = e;
            return -1;
        }
    }
    return -1;
}

#endif /* TK_BIND_H */
