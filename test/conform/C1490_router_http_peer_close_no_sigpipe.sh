#!/usr/bin/env bash
# C1490_router_http_peer_close_no_sigpipe.sh — router.serve's plain HTTP
# response written to a client that has already closed must fail quietly,
# never kill the server with SIGPIPE (story 127.350).
#
# THE DEFECT.  127.347 guarded the router's WebSocket upgrade path only.
# router_send_response() wrote the status line, headers and body through
# write_all(), which used plain write(2), and router_serve() never set
# SO_NOSIGPIPE on an accepted socket.  A client that sent a request and closed
# before reading the answer killed the whole router.serve process (status
# 141).  The fix routes write_all() through tk_send_all() (send with
# MSG_NOSIGNAL where it exists; write(2) on ENOTSOCK, so router_handle_fd
# still serves a pipe) and sets tk_sock_nosigpipe() on every accepted socket.
#
# WHY IT IS DETERMINISTIC.  No clock and no race, as C1302: a C probe writes
# the request into one end of an AF_UNIX socketpair, CLOSES that end, then
# hands the other to router_handle_fd().  The request is still readable, and
# the response write meets a peer that is already gone, which on AF_UNIX is
# EPIPE every time.  SIGPIPE is at its default disposition in the probe.
#   unrouted — GET with no matching route: the 404 response.
#   routed   — GET /big, a 256 KiB body: header and body writes.
#   live     — the same /hi request on a socketpair whose client only shut
#              down its write side: the response still arrives, so the send
#              path carries real traffic.
#
# Negative control: C1490_STDLIB=<dir holding e31aac45's src/stdlib> — the
# unrouted and routed cases are killed (rc 141); see the commit.
#
# Story: 127.350

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SL="${C1490_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1490_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

cat > probe.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include "router.h"
static char *big;
static TkRouteResp h_hi(TkRouteCtx ctx)  { (void)ctx; return router_resp_ok("hi", "text/plain"); }
static TkRouteResp h_big(TkRouteCtx ctx) { (void)ctx; return router_resp_ok(big, "text/plain"); }
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "unrouted";
    const char *path = strcmp(mode, "routed") == 0 ? "/big"
                     : strcmp(mode, "live") == 0   ? "/hi" : "/nothing";
    char req[256];
    int sv[2];
    signal(SIGPIPE, SIG_DFL);
    big = malloc(256 * 1024 + 1);
    if (!big) return 2;
    memset(big, 'x', 256 * 1024); big[256 * 1024] = '\0';
    TkRouter *r = router_new();
    router_get(r, "/hi", h_hi);
    router_get(r, "/big", h_big);
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 3;
    if (write(sv[1], req, (size_t)n) != n) return 4;
    if (strcmp(mode, "live") == 0) {
        shutdown(sv[1], SHUT_WR);          /* the client still reads */
        router_handle_fd(r, sv[0]);
        char buf[512];
        ssize_t got = read(sv[1], buf, sizeof buf - 1);   /* one bounded read */
        buf[got > 0 ? got : 0] = '\0';
        char *eol = strstr(buf, "\r\n");
        if (eol) *eol = '\0';
        printf("survived %s\n", buf);
        return 0;
    }
    close(sv[1]);                          /* the client is gone before the reply */
    router_handle_fd(r, sv[0]);            /* reads the request, writes the reply */
    printf("survived\n");
    return 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects (C696, C703).
CF=(-std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror -iquote "${SL}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${SL}" --tkc "${TKC:-${REPO_ROOT}/tkc}" --cc cc --objects router ws -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set"; echo "C1490: 0 passed, 1 failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! cc "${CF[@]}" -o probe probe.c "${LS[@]}" -lz > build.log 2>&1; then
    echo "FAIL: the probe does not build"; head -8 build.log; echo "C1490: 0 passed, 1 failed"; exit 1
fi
out="$(trap - PIPE; ./probe unrouted 2>&1)"; rc=$?
expect "1-unrouted-GET-to-a-closed-client-404-write-does-not-kill" "rc=${rc} ${out}" "rc=0 survived"
out="$(trap - PIPE; ./probe routed 2>&1)"; rc=$?
expect "2-routed-GET-to-a-closed-client-256KiB-body-write-does-not-kill" "rc=${rc} ${out}" "rc=0 survived"
out="$(trap - PIPE; ./probe live 2>&1)"; rc=$?
expect "3-a-live-client-still-gets-the-response" "rc=${rc} ${out}" "rc=0 survived HTTP/1.1 200 OK"

echo "C1490: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
