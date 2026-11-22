/* SPDX-License-Identifier: MIT */
/*
 * net_glue.c — i64-ABI wrappers for std.net module.
 *
 * Story 114.31: these wrappers previously lived in tk_web_glue.c, which is only
 * linked when std.http is imported — so a bare `i=net:std.net` program passed
 * --check but failed to link (`_tk_net_portavailable_w` undefined). Moved here
 * and added to the net module's c_files so std.net links standalone. (http
 * depends on net via stdlib_deps, so it still gets these — no duplication.)
 */

#include "net.h"
#include "capabilities.h"   /* 124.4c: net capability gate */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tk_bind.h"   /* 135.33 */
#include "tk_strhdr.h" /* 127.170: net.read returns a sealed string */
#include "tk_array.h"   /* 138.25: net.resolve returns [str] */
#include "tk_runtime.h" /* 138.25: tk_current_error, tk_err_box, tk_err_msg */
#include <netdb.h>      /* gai_strerror */

int64_t tk_net_portavailable_w(int64_t port) {
    /* 138.19: net_portavailable binds 127.0.0.1:port to find out -- a listen. */
    TK_REQUIRE_NET(TK_CAP_NET_LISTEN, "127.0.0.1", port);
    return (int64_t)net_portavailable((uint64_t)port);
}

/* net.listen(addr) — a listening TCP socket on "host:port" (IPv4), or 0.
 *
 * 138.13 (loke T-49): `net.listen(11444)` passed a port where the glue reads
 * a C string, and strrchr() dereferenced address 11444 (SIGSEGV).  The
 * interface now declares the parameter `str`; until the checker rejects an
 * integer there (reported to GA), a value below 65536 -- never a valid
 * pointer on a supported platform -- is taken as the PORT and bound to
 * 127.0.0.1, the loopback address, never to every interface. */
int64_t tk_net_listen_w(int64_t addr) {
    char portaddr[32];
    if (addr > 0 && addr < 65536) {
        snprintf(portaddr, sizeof portaddr, "127.0.0.1:%d", (int)addr);
        addr = (int64_t)(intptr_t)portaddr;
    }
    const char *s = addr > 0 ? (const char *)(intptr_t)addr : NULL;

    /* Parse "host:port" — find last ':' to split.  138.19: parsed first, so
     * the listen gate sees the address and port it would bind (a value that
     * does not parse binds nothing, and under a scoped grant is denied). */
    const char *colon = s ? strrchr(s, ':') : NULL;
    char host[256] = "";
    int port = 0;
    if (colon && colon != s && (size_t)(colon - s) < sizeof host) {
        memcpy(host, s, (size_t)(colon - s));
        host[colon - s] = '\0';
        port = atoi(colon + 1);
    }
    TK_REQUIRE_NET(TK_CAP_NET_LISTEN, host[0] ? host : s, port > 0 ? port : 0);
    if (!s || !host[0]) return 0;         /* null, or no port / address at all */
    if (port <= 0 || port > 65535) return 0;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1)
        return 0;

    /* 135.33: never shadow a live listener on the same port (tk_bind.h). */
    int fd = tk_bind_exclusive(&sa);
    if (fd < 0) return 0;

    if (listen(fd, 128) < 0) {
        close(fd);
        return 0;
    }

    return (int64_t)fd;
}

int64_t tk_net_accept_w(int64_t listener) {
    /* 138.19: accepting on a listener is listen authority on the address it
     * is bound to, so a listen:ADDR:PORT scope that admitted net.listen
     * admits its accepts. */
    char la[TK_CAP_ADDRSTRLEN];
    uint64_t lport = 0;
    const char *lhost = tk_cap_sockname((int)listener, la, &lport);
    TK_REQUIRE_NET(TK_CAP_NET_LISTEN, lhost, lport);
    int fd = accept((int)listener, NULL, NULL);
    if (fd < 0) return 0;
    return (int64_t)fd;
}

int64_t tk_net_read_w(int64_t conn) {
    char tmp[4096];
    ssize_t n = read((int)conn, tmp, sizeof tmp - 1);
    if (n <= 0) return 0;
    /* 127.170: sealed with the C-string length (the bytes may hold a 0) */
    const char *nul = memchr(tmp, '\0', (size_t)n);
    char *buf = tks_dup(tmp, nul ? (size_t)(nul - tmp) : (size_t)n);
    return (int64_t)(intptr_t)buf;
}

int64_t tk_net_write_w(int64_t conn, int64_t data) {
    const char *s = (const char *)(intptr_t)data;
    if (!s) return 0;
    ssize_t n = write((int)conn, s, tk_strlen(s));
    return (n < 0) ? 0 : (int64_t)n;
}

int64_t tk_net_close_w(int64_t conn) {
    close((int)conn);
    return 0;
}

/* net.resolve(name) -> [str]!neterr (138.25, owner answer 2.2).
 *
 * Every address `name` has, A and AAAA, as numeric strings, so that a
 * caller can check each one (private, loopback, link-local, metadata
 * addresses) and then connect to the one it checked with
 * http.withconnectaddr: resolving inside the connect, as the http client
 * does, lets the name answer differently the second time (DNS rebinding).
 *
 * Gated by `net`: a lookup is network traffic, and getaddrinfo is a net
 * primitive to scripts/check_capability_sinks.py.  The error is a neterr
 * {msg; code} box in tk_current_error (runtime-abi §7), code 0. */
static int64_t net_raise(const char *msg)
{
    TkNetErrRec *box = (TkNetErrRec *)tk_err_box((int64_t)sizeof *box);
    box->msg  = tk_err_msg("%s", msg ? msg : "");
    box->code = 0;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

int64_t tk_net_resolve_w(int64_t name) {
    TK_REQUIRE(TK_CAP_NET);
    const char *nm = (const char *)(intptr_t)name;
    if (!nm || !nm[0]) return net_raise("net.resolve: empty name");
    NetAddrs a = net_resolve(nm);
    if (a.err) {
        char m[320];
        snprintf(m, sizeof m, "net.resolve: %s: %s", nm, gai_strerror(a.err));
        return net_raise(m);
    }
    int64_t h = tk_arr_alloc((int64_t)a.n, (int64_t)a.n);
    if (!h) { net_addrs_free(&a); return net_raise("net.resolve: out of memory"); }
    int64_t *block = (int64_t *)(intptr_t)h;
    for (size_t i = 0; i < a.n; i++) {
        char *s = tks_strdup(a.addrs[i]);   /* sealed (127.170) */
        if (!s) {
            /* the block from tk_arr_alloc starts TK_ARR_HDR words before
             * its handle; the strings already sealed are never freed
             * (runtime-abi §3.1) */
            free(block - TK_ARR_HDR);
            net_addrs_free(&a);
            return net_raise("net.resolve: out of memory");
        }
        block[i] = (int64_t)(intptr_t)s;
    }
    net_addrs_free(&a);
    tk_current_error = 0;
    return h;
}
