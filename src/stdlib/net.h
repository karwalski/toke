/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_NET_H
#define TK_STDLIB_NET_H

/*
 * net.h — C interface for the std.net standard library module.
 *
 * Type mappings:
 *   int (port)  = uint64_t
 *   int (result) = int  (1 = available, 0 = in use or invalid)
 *
 * Story: 42.1.1
 */

#include <stdint.h>
#include <stddef.h>

/*
 * net_portavailable — check whether a TCP port is available on 127.0.0.1.
 *
 * Attempts to bind a SOCK_STREAM socket to 127.0.0.1:port.
 * Returns 1 if the port is free (bind succeeds), 0 if in use or invalid.
 * Invalid ports (0 or >65535) return 0 without attempting a bind.
 */
int net_portavailable(uint64_t port);

/*
 * net_resolve — every address `name` resolves to (138.25).
 *
 * One getaddrinfo() call, any family (A and AAAA), each address once, as a
 * numeric string ("127.0.0.1", "::1") in the order the resolver returned
 * them.  On success err is 0 and addrs holds n >= 1 malloc'd strings; on
 * failure err is the EAI_* code (gai_strerror names it), or EAI_MEMORY.
 * Free with net_addrs_free.  The capability check is the caller's
 * (tk_net_resolve_w).
 */
typedef struct {
    char  **addrs;
    size_t  n;
    int     err;
} NetAddrs;

NetAddrs net_resolve(const char *name);
void     net_addrs_free(NetAddrs *a);

/* tki-record: net.neterr — the $err of net.resolve, shaped as std.http's
 * httperr: msg, and code (0: a failure with no number of its own). */
typedef struct {
    const char *msg;
    int64_t     code;
} TkNetErrRec;

#endif /* TK_STDLIB_NET_H */
