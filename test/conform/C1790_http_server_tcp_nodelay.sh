#!/usr/bin/env bash
# C1790_http_server_tcp_nodelay.sh — std.http turns Nagle off on the
# connections it serves (story 127.443).
#
# THE DEFECT.  res_write_fd() sends a response in several writes (status
# line, the handler's fields, security and CORS fields, blank line, body).
# The accepted socket kept Nagle on, so every write after the first waited
# for the client's ACK of the first, and a client waiting for the whole
# response delays that ACK: about 40 ms per keep-alive response on Linux.
# S005's 7,264 loopback requests ran into its 180 s alarm on ubuntu-latest.
#
# THE FIX.  handle_connection() and handle_tls_connection() set TCP_NODELAY
# on the connection (http_sock_nodelay in src/stdlib/http.c).
#
# CASES (one C harness, the real server loop via http_handle_fd):
#   1  control: the socket the harness accepted has TCP_NODELAY off, so
#      case 2 measures the runtime, not the platform default
#   2  inside a handler, the connection being served has TCP_NODELAY on
#   3  300 keep-alive requests on one connection finish in under 6 s
#      (a 40 ms stall each would take 12 s; unstalled it is well under 1 s)
#
# Story: 127.443

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1790_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1790: std.http serves its connections with TCP_NODELAY"
echo "--------------------------------------------------------"

cat > "${WORK}/h.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "http.h"

static int g_port, g_conn_fd = -1, g_seen = -1;

static int nodelay(int fd)
{
    int v = 0; socklen_t l = sizeof v;
    if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, &l) != 0) return -1;
    return v != 0;
}

static Res h(Req req)
{
    (void)req;
    if (g_seen < 0) g_seen = nodelay(g_conn_fd);
    return http_Res_ok("ok");
}

static const char REQ[] =
    "GET /n HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";

static void *client(void *arg)
{
    long n = (long)(intptr_t)arg;
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a) != 0) return (void *)1;
    for (long i = 0; i < n; i++) {
        if (write(s, REQ, sizeof REQ - 1) != (ssize_t)(sizeof REQ - 1)) { close(s); return (void *)1; }
        char buf[4096]; size_t have = 0; long want = -1;
        for (;;) {
            ssize_t r = read(s, buf + have, sizeof buf - 1 - have);
            if (r <= 0) { close(s); return (void *)1; }
            have += (size_t)r; buf[have] = 0;
            if (want < 0) {
                char *sep = strstr(buf, "\r\n\r\n");
                if (!sep) continue;
                char *cp = strstr(buf, "Content-Length:");
                want = (long)(sep - buf) + 4 + (cp ? atol(cp + 15) : 0);
            }
            if ((long)have >= want) break;
        }
    }
    close(s);
    return (void *)0;
}

int main(void)
{
    alarm(60);
    struct sockaddr_in a; socklen_t al = sizeof a;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 4) != 0 ||
        getsockname(ls, (struct sockaddr *)&a, &al) != 0) { perror("listen"); return 2; }
    g_port = ntohs(a.sin_port);
    http_GET("/n", h);

    const long N = 300;
    pthread_t t; void *rc = NULL;
    pthread_create(&t, NULL, client, (void *)(intptr_t)N);
    g_conn_fd = accept(ls, NULL, NULL);
    if (g_conn_fd < 0) { perror("accept"); return 2; }
    printf("before=%d\n", nodelay(g_conn_fd));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    http_handle_fd(g_conn_fd);           /* closes the fd */
    pthread_join(t, &rc);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    printf("during=%d\nclient=%s\nms=%ld\n", g_seen, rc == NULL ? "ok" : "failed", ms);
    return 0;
}
EOF

CF=(-std=c99 -D_GNU_SOURCE -g -Wall -Wextra -Werror -Wno-unused-function -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" --objects http -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "C1790: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" \
        -lz -lpthread -lm >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: the harness does not build"; grep -a -m4 -E 'undefined reference|error' "${WORK}/cc.log" | sed 's/^/      | /'
    echo "C1790: 0 passed, 1 failed"; exit 1
fi

out="$(cd "${WORK}" && ./h 2>"${WORK}/h.err")"; rc=$?
val() { printf '%s\n' "${out}" | sed -n "s/^$1=//p"; }
if [ "${rc}" -ne 0 ]; then
    fail "the harness exited ${rc}: $(head -c 300 "${WORK}/h.err")"
else
    [ "$(val before)" = "0" ] && pass "1 control: the accepted socket starts with Nagle on" \
        || fail "1 control: TCP_NODELAY was '$(val before)' before std.http saw the socket (want 0)"
    [ "$(val during)" = "1" ] && pass "2 the connection a handler is served on has TCP_NODELAY" \
        || fail "2 TCP_NODELAY during the handler: '$(val during)' (want 1)"
    ms="$(val ms)"
    if [ "$(val client)" = "ok" ] && [ -n "${ms}" ] && [ "${ms}" -lt 6000 ]; then
        pass "3 300 keep-alive responses in ${ms} ms (< 6000)"
    else
        fail "3 300 keep-alive responses: client $(val client), ${ms:-?} ms (want ok, < 6000; 40 ms each is 12000)"
    fi
fi

echo "--------------------------------------------------------"
echo "C1790: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
