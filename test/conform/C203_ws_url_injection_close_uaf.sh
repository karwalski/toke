#!/usr/bin/env bash
# C203_ws_url_injection_close_uaf.sh — ws_connect refuses a URL that would
# inject into its upgrade request, and a closed $wsconn is safe to use
# (stories 122.13 (5), 135.20 (1)).
#
# THE DEFECTS (measured on main before this story).
#   URL injection  ws_parse_url took the path as "the rest of the string" and
#                  the host as everything up to ':' or '/', with no check for
#                  CR, LF, space or other control bytes; ws_connect wrote both
#                  verbatim into "GET <path> HTTP/1.1\r\nHost: <host>...".
#                  ws://127.0.0.1:P/x HTTP/1.1\r\nX-Injected: yes\r\n... put
#                  a header of the caller's choosing on the wire (verified:
#                  the listener below receives it).  A port past 65535 was
#                  truncated to 16 bits rather than refused.
#   close UAF      tk_ws_close_w closed the connection and then free()d the
#                  WsConn, while the toke program still held the handle; the
#                  documented "close, then send takes $err" pattern made
#                  ws_conn_valid read the freed struct (ASan
#                  heap-use-after-free, seen in C107 under ASan).
#
# WHAT THIS DRIVES (ASan+UBSan build of ws.c + ws_glue.c):
#   a forked ws_connect to a local listener with each hostile URL; the
#   parent records what arrived.  Fixed: ws_connect fails before connecting,
#   nothing arrives.  A clean URL (control) does connect and send a normal
#   request line.  Then a WsConn on a socketpair is closed through
#   tk_ws_close_w and driven through send, sendbytes, recv and close again:
#   every call returns its error and ASan sees no access to freed memory.
#
# Negative control: C203_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's ws.c and ws_glue.c fails the injection checks and aborts
# with heap-use-after-free on the first call after close.
#
# Story: 122.13, 135.20

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C203_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c203_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C203: ws URL cannot inject into the upgrade request; closed conn is safe"
echo "-----------------------------------------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ws.h"
#include "bytes_rt.h"

int64_t tk_ws_close_w(int64_t conn);
int64_t tk_ws_send_w(int64_t conn, int64_t msg);
int64_t tk_ws_sendbytes_w(int64_t conn, int64_t data);
int64_t tk_ws_recv_w(int64_t conn);

/* 127.238: ws_glue.c fills the error slot (tk_current_error + tk_err_box).
 * 127.268: those come from the real tk_runtime.c, which the link set below
 * carries -- the stand-ins this file used to define would now collide. */
#include "tk_runtime.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* Run ws_connect(url) in a child against a listener; return what the
 * listener received within 2 s (malloc'd, "" if nothing), and the child's
 * verdict in *connected (1 = ws_connect reported success). */
static char *probe(int lfd, const char *url, int *connect_err) {
    pid_t pid = fork();
    if (pid == 0) {
        WsConnResult r = ws_connect(url);
        _exit(r.is_err ? 1 : 0);
    }
    char *got = calloc(1, 8192); size_t n = 0;
    struct pollfd p = { lfd, POLLIN, 0 };
    if (poll(&p, 1, 2000) > 0) {
        int c = accept(lfd, NULL, NULL);
        if (c >= 0) {
            struct pollfd q = { c, POLLIN, 0 };
            while (n < 8191 && poll(&q, 1, 500) > 0) {
                ssize_t k = read(c, got + n, 8191 - n);
                if (k <= 0) break;
                n += (size_t)k;
                if (strstr(got, "\r\n\r\n")) break;
            }
            close(c);                      /* no reply: the child's handshake fails */
        }
    }
    int st = 0;
    waitpid(pid, &st, 0);
    *connect_err = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
    return got;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t al = sizeof a;
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 4) ||
        getsockname(lfd, (struct sockaddr *)&a, &al)) { puts("  FAIL: listener"); return 1; }
    int port = ntohs(a.sin_port);
    char url[512]; int err;

    snprintf(url, sizeof url, "ws://127.0.0.1:%d/chat", port);
    char *got = probe(lfd, url, &err);
    CHECK(strncmp(got, "GET /chat HTTP/1.1\r\n", 20) == 0,
          "control: a clean URL connects and sends 'GET /chat HTTP/1.1'");
    free(got);

    snprintf(url, sizeof url,
             "ws://127.0.0.1:%d/x HTTP/1.1\r\nX-Injected: yes\r\nHost: evil\r\n\r\nGET /y", port);
    got = probe(lfd, url, &err);
    CHECK(strstr(got, "X-Injected") == NULL && err == 1,
          "CR/LF in the path: refused, no injected header reaches the server");
    free(got);

    snprintf(url, sizeof url, "ws://127.0.0.1:%d/a b", port);
    got = probe(lfd, url, &err);
    CHECK(got[0] == '\0' && err == 1, "a space in the path (a second request-line token) is refused");
    free(got);

    close(lfd);

    /* A port that truncates to a listening one: bind P in 20000..34463, ask
     * for P + 65536 (still five digits).  Truncated, it connects to P. */
    int lf2 = socket(AF_INET, SOCK_STREAM, 0), p2 = 0;
    for (int tryp = 20000 + (int)(getpid() % 9000); tryp < 34463 && !p2; tryp += 97) {
        a.sin_port = htons((uint16_t)tryp);
        if (bind(lf2, (struct sockaddr *)&a, sizeof a) == 0 && listen(lf2, 4) == 0) p2 = tryp;
    }
    if (p2) {
        snprintf(url, sizeof url, "ws://127.0.0.1:%d/", p2 + 65536);
        got = probe(lf2, url, &err);
        CHECK(got[0] == '\0' && err == 1, "port P+65536 is refused, not truncated to 16 bits (to P)");
        free(got);
    } else {
        puts("  SKIP: no free port in 20000..34463 for the truncation check");
    }
    close(lf2);

    /* ---- closed handle ---- */
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp)) { puts("  FAIL: socketpair"); return 1; }
    WsConn *c = malloc(sizeof *c);
    c->magic = WS_CONN_MAGIC; c->fd = sp[0]; c->mask = 1; c->closed = 0;
    int64_t h = (int64_t)(intptr_t)c;
    tk_ws_close_w(h);
    uint8_t one = 7;
    CHECK(tk_ws_sendbytes_w(h, tk_bytes_pack(&one, 1)) == -1, "sendbytes after close is an error (no use-after-free)");
    CHECK(tk_ws_send_w(h, (int64_t)(intptr_t)"hi") == -1, "send after close is an error");
    CHECK(tk_ws_recv_w(h) == 0, "recv after close is an error");
    tk_ws_close_w(h);
    CHECK(1, "a second close is harmless");
    close(sp[1]);

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" ws)" \
    || { echo "  FAIL: stdlib link set: see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" \
        "${LINK[@]}" -lz -lm -lpthread >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" 2>"${WORK}/err.log"
rc=$?
grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err.log" | sed 's/^/  /'
echo ""
[ "${rc}" -eq 0 ] && echo "C203: PASS" || echo "C203: FAIL (exit ${rc})"
exit "${rc}"
