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
