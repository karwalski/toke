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
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int64_t tk_net_portavailable_w(int64_t port) {
    TK_REQUIRE(TK_CAP_NET);
    return (int64_t)net_portavailable((uint64_t)port);
}

int64_t tk_net_listen_w(int64_t addr) {
    TK_REQUIRE(TK_CAP_NET);
    const char *s = (const char *)(intptr_t)addr;
    if (!s) return 0;

    /* Parse "host:port" — find last ':' to split. */
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s) return 0;

    char host[256];
    size_t hlen = (size_t)(colon - s);
    if (hlen >= sizeof(host)) return 0;
    memcpy(host, s, hlen);
    host[hlen] = '\0';

    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close(fd);
        return 0;
    }

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return 0;
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        return 0;
    }

    return (int64_t)fd;
}

int64_t tk_net_accept_w(int64_t listener) {
    TK_REQUIRE(TK_CAP_NET);
    int fd = accept((int)listener, NULL, NULL);
    if (fd < 0) return 0;
    return (int64_t)fd;
}

int64_t tk_net_read_w(int64_t conn) {
    char *buf = malloc(4096);
    if (!buf) return 0;
    ssize_t n = read((int)conn, buf, 4095);
    if (n <= 0) { free(buf); return 0; }
    buf[n] = '\0';
    return (int64_t)(intptr_t)buf;
}

int64_t tk_net_write_w(int64_t conn, int64_t data) {
    const char *s = (const char *)(intptr_t)data;
    if (!s) return 0;
    ssize_t n = write((int)conn, s, strlen(s));
    return (n < 0) ? 0 : (int64_t)n;
}

int64_t tk_net_close_w(int64_t conn) {
    close((int)conn);
    return 0;
}
