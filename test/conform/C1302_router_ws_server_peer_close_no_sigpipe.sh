#!/usr/bin/env bash
# C1302_router_ws_server_peer_close_no_sigpipe.sh — the router's WebSocket
# server writes (101, 404, 400, 500, PONG, CLOSE reply) to a client that has
# already closed must fail quietly, never kill the server with SIGPIPE
# (story 127.347, the server half).
#
# THE DEFECT.  router_ws_try_upgrade() in src/stdlib/router.c wrote every
# reply with plain write(2) on an accepted socket that never had SO_NOSIGPIPE,
# so a browser that sent an upgrade and closed the tab before reading the 101
# killed the whole router.serve process (status 141).  The fix sets the same
# per-socket guard the http server uses (SO_NOSIGPIPE where it exists) and
# sends every reply through tk_send_all (MSG_NOSIGNAL where it exists).
#
# WHY IT IS DETERMINISTIC.  No clock and no race: a C probe writes the request
# into one end of an AF_UNIX socketpair, CLOSES that end, then hands the other
# to router_handle_fd().  The request is still readable, and the reply write
# meets a peer that is already gone, which on AF_UNIX is EPIPE every time.
# SIGPIPE is at its default disposition in the probe.
#   upgrade  — a registered ws route: the 101 write.
#   unrouted — an upgrade to a path with no ws route: the 404 write.
#
# Negative control: C1302_STDLIB=<dir holding 75aa44a0's src/stdlib> — both
# cases are killed (rc 141); see the commit.
#
# Story: 127.347

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SL="${C1302_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1302_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

cat > probe.c <<'C'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include "router.h"
static int opened;
static void on_open(int fd) { (void)fd; opened = 1; }
static void on_msg(int fd, const char *d, size_t n, int b) { (void)fd; (void)d; (void)n; (void)b; }
static void on_close(int fd) { (void)fd; }
int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/ws";
    char req[512];
    int sv[2];
    signal(SIGPIPE, SIG_DFL);
    TkRouter *r = router_new();
    router_ws(r, "/ws", on_open, on_msg, on_close);
    int n = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", path);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
    if (write(sv[1], req, (size_t)n) != n) return 3;
    close(sv[1]);                        /* the client is gone before the reply */
    router_handle_fd(r, sv[0]);          /* reads the request, writes the reply */
    printf("survived open=%d\n", opened);
    return 0;
}
C
if ! cc -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror -iquote "${SL}" -o probe probe.c \
        "${SL}/router.c" "${SL}/ws.c" "${SL}/str.c" "${SL}/capabilities.c" -lz > build.log 2>&1; then
    echo "FAIL: the probe does not build"; head -8 build.log; echo "C1302: 0 passed, 1 failed"; exit 1
fi
out="$(trap - PIPE; ./probe /ws 2>&1)"; rc=$?
expect "1-upgrade-to-a-closed-client-101-write-does-not-kill" "rc=${rc} ${out}" "rc=0 survived open=1"
out="$(trap - PIPE; ./probe /nope 2>&1)"; rc=$?
expect "2-unrouted-upgrade-to-a-closed-client-404-write-does-not-kill" "rc=${rc} ${out}" "rc=0 survived open=0"

echo "C1302: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
