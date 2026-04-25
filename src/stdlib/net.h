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

/*
 * net_portavailable — check whether a TCP port is available on 127.0.0.1.
 *
 * Attempts to bind a SOCK_STREAM socket to 127.0.0.1:port.
 * Returns 1 if the port is free (bind succeeds), 0 if in use or invalid.
 * Invalid ports (0 or >65535) return 0 without attempting a bind.
 */
int net_portavailable(uint64_t port);

#endif /* TK_STDLIB_NET_H */
