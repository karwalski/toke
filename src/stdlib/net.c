/* SPDX-License-Identifier: MIT */
/*
 * net.c — Implementation of the std.net standard library module.
 *
 * Story: 42.1.1
 */

#include "net.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdlib.h>
#include <netdb.h>
#include "tk_bind.h"   /* 135.33 */

int net_portavailable(uint64_t port)
{
    if (port == 0 || port > 65535)
        return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    /* 135.33: with SO_REUSEADDR set up front, a 127.0.0.1 bind succeeded on
     * macOS beside a live *:P listener, so a port in use was reported free.
     * tk_bind_exclusive() fails when a listener answers, and still reports a
     * port whose old connections sit in TIME_WAIT as free -- a server can
     * bind it. */
    int fd = tk_bind_exclusive(&addr);
    if (fd < 0)
        return 0; /* port in use (EADDRINUSE) or other error */
    close(fd);
    return 1;     /* port is free */
}

/* 138.25: see net.h.  No AI_ADDRCONFIG: the caller asked for every address
 * the name has, not those this host could reach today. */
NetAddrs net_resolve(const char *name)
{
    NetAddrs a;
    memset(&a, 0, sizeof a);
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;     /* one entry per address, not per socket type */
    int rc = getaddrinfo(name, NULL, &hints, &res);
    if (rc != 0) { a.err = rc; return a; }
    size_t cap = 0;
    for (rp = res; rp; rp = rp->ai_next) cap++;
    a.addrs = (char **)calloc(cap ? cap : 1, sizeof(char *));
    if (!a.addrs) { freeaddrinfo(res); a.err = EAI_MEMORY; return a; }
    for (rp = res; rp; rp = rp->ai_next) {
        char buf[INET6_ADDRSTRLEN];
        const void *src;
        if (rp->ai_family == AF_INET)
            src = &((const struct sockaddr_in *)(const void *)rp->ai_addr)->sin_addr;
        else if (rp->ai_family == AF_INET6)
            src = &((const struct sockaddr_in6 *)(const void *)rp->ai_addr)->sin6_addr;
        else
            continue;
        if (!inet_ntop(rp->ai_family, src, buf, sizeof buf)) continue;
        int dup = 0;
        for (size_t i = 0; i < a.n; i++)
            if (strcmp(a.addrs[i], buf) == 0) { dup = 1; break; }
        if (dup) continue;
        const char *z = memchr(buf, '\0', sizeof buf);   /* inet_ntop terminated it */
        size_t l = (size_t)(z - buf) + 1;
        char *s = (char *)malloc(l);
        if (!s) { freeaddrinfo(res); net_addrs_free(&a); a.err = EAI_MEMORY; return a; }
        memcpy(s, buf, l);
        a.addrs[a.n++] = s;
    }
    freeaddrinfo(res);
    if (a.n == 0) { net_addrs_free(&a); a.err = EAI_NONAME; }
    return a;
}

void net_addrs_free(NetAddrs *a)
{
    if (!a) return;
    for (size_t i = 0; i < a->n; i++) free(a->addrs[i]);
    free(a->addrs);
    a->addrs = NULL;
    a->n = 0;
}
