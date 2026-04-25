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

int net_portavailable(uint64_t port)
{
    if (port == 0 || port > 65535)
        return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    int result;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        result = 1; /* port is free */
    } else {
        result = 0; /* port in use (EADDRINUSE) or other error */
    }

    close(fd);
    return result;
}
