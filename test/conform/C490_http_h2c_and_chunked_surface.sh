#!/usr/bin/env bash
# C490_http_h2c_and_chunked_surface.sh — two residues of Batch H3's request
# reader (story 121.44 items 1 and 2).
#
# 1. h2c UPGRADE.  A request carrying "Upgrade: h2c" got a 101 and was then
#    handed to handle_h2_connection(fd, NULL, ...), which read the connection
#    preface with SSL_read() on a NULL SSL -- so the read failed at once and
#    the connection closed.  Measured on main (1346588c) with this harness:
#    "101 Switching Protocols" and nothing else, whether the client's HTTP/2
#    bytes arrived in the same segment as the upgrade request (where the
#    reader had buffered them and they were dropped) or 200 ms later.  The
#    upgrade request itself was never answered, although RFC 7540 3.2
#    requires its response on stream 1.  The path has never served a
#    request, and RFC 9113 3.1 deprecates the h2c upgrade.  It is retired:
#    the server ignores the Upgrade header, as RFC 9110 7.8 allows, and
#    answers over HTTP/1.1 -- so bytes pipelined behind the upgrade request
#    stay in the shared reader and are served as the next request.  HTTP/2
#    remains available over TLS via ALPN (C491).
#
# 2. http_chunked_read(fd) read a chunked body FRESH from the socket, so it
#    could not see bytes already buffered with the headers (the HTT-02 shape)
#    and consumed the peer's stream a byte at a time with no deadline.  No
#    server path had called it since 121.17; it stayed exported from http.h
#    for any C caller to pick up.  It is removed: the server decodes chunked
#    bodies with the shared reader, http_read_chunked().
#
# WHAT THIS DRIVES.  (1) a C harness that #includes src/stdlib/http.c built
# WITH TK_HAVE_OPENSSL (the h2c branch only exists there), under
# ASan+UBSan, over a real loopback TCP connection to handle_connection().
# Without OpenSSL headers that part is a named SKIP.  (2) the header and the
# compiled object: http.h must not declare http_chunked_read and http.o must
# not define it.
#
# Negative control: C490_STDLIB=<dir> points both parts at another stdlib
# tree; main's src/stdlib fails every case below except the plain control.
#
# Story: 121.44

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C490_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c490_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C490: h2c upgrade and the http_chunked_read surface"
echo "---------------------------------------------------"

# ── 2. http_chunked_read is gone from the header and the object ──────────
if /usr/bin/grep -a -q -E 'char[[:space:]]*\*[[:space:]]*http_chunked_read' "${S}/http.h"; then
    fail "http.h still declares http_chunked_read"
else
    pass "http.h does not declare http_chunked_read"
fi
if "${CC}" -std=c99 -D_GNU_SOURCE -c -iquote "${S}" -I"${S}" -o "${WORK}/http.o" \
        "${S}/http.c" >"${WORK}/obj.cc" 2>&1; then
    if nm -g "${WORK}/http.o" 2>/dev/null | awk '{print $NF}' \
            | /usr/bin/grep -a -q -E '^_?http_chunked_read$'; then
        fail "http.o still exports http_chunked_read"
    else
        pass "http.o does not export http_chunked_read"
    fi
    if nm -g "${WORK}/http.o" 2>/dev/null | awk '{print $NF}' \
            | /usr/bin/grep -a -q -E '^_?http_chunked_write$'; then
        pass "control: http.o still exports http_chunked_write"
    else
        fail "control: http_chunked_write missing from http.o (nm unusable?)"
    fi
else
    fail "http.o: build: $(grep -a -m3 error "${WORK}/obj.cc" | head -c 300)"
fi

# ── 1. h2c upgrade ───────────────────────────────────────────────────────
OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
if [ -z "${OSSL_INC}" ]; then
    echo "  SKIP: h2c -- the Upgrade branch is compiled only with OpenSSL, and this host has no headers"
else
OSSL_LIB="$(dirname "${OSSL_INC}")/lib"
cat > "${WORK}/h.c" <<'EOF'
#include "http.c"
#include <pthread.h>

static char g_echo[512];
static Res echo(Req r) {
    snprintf(g_echo, sizeof g_echo, "body=%s", r.body ? r.body : "");
    return make_res(200, g_echo);
}
static Res ping(Req r) { (void)r; return make_res(200, "pong"); }

typedef struct { const char *name; const char *seg[3]; int gap_ms;
                 const char *want_status; const char *want_text[2]; } Case;
static int g_port, g_lfd;
typedef struct { const Case *c; char out[16384]; size_t n; } Client;

#define UP "Connection: Upgrade, HTTP2-Settings\r\nUpgrade: h2c\r\n" \
           "HTTP2-Settings: AAMAAABkAAQAoAAAAAIAAAAA\r\n"
#define PRI "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

static void *client(void *arg) {
    Client *cl = arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return NULL; }
    struct timeval tv = { 6, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (int i = 0; i < 3 && cl->c->seg[i]; i++) {
        if (i && cl->c->gap_ms) usleep((useconds_t)cl->c->gap_ms * 1000);
        if (send(s, cl->c->seg[i], strlen(cl->c->seg[i]), 0) <= 0) break;
    }
    shutdown(s, SHUT_WR);
    for (;;) {
        size_t room = sizeof cl->out - 1 - cl->n;
        if (!room) break;
        ssize_t r = recv(s, cl->out + cl->n, room, 0);
        if (r <= 0) break;
        cl->n += (size_t)r;
    }
    cl->out[cl->n] = '\0';
    close(s);
    return NULL;
}

static void statuses(const char *o, char *dst, size_t cap) {
    dst[0] = '\0';
    for (const char *p = o; (p = strstr(p, "HTTP/1.1 ")); p += 9) {
        if (strlen(p) < 12 || p[12] != ' ') continue;
        size_t l = strlen(dst);
        snprintf(dst + l, cap - l, "%s%.3s", l ? "," : "", p + 9);
    }
}

static const Case CASES[] = {
  { "upgrade-then-pipelined-get-same-segment",
    { "GET /ping HTTP/1.1\r\nHost: t\r\n" UP "\r\n"
      "GET /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", NULL }, 0,
    "200,200", { "pong", NULL } },
  { "upgrade-post-with-body-then-pipelined-get",
    { "POST /echo HTTP/1.1\r\nHost: t\r\n" UP "Content-Length: 5\r\n\r\nhello"
      "GET /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", NULL }, 0,
    "200,200", { "body=hello", "pong" } },
  { "upgrade-with-h2-preface-carried-is-not-switched",
    { "GET /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n" UP "\r\n" PRI, NULL }, 0,
    "200", { "pong", NULL } },
  { "upgrade-alone",
    { "GET /ping HTTP/1.1\r\nHost: t\r\n" UP "\r\n", NULL }, 0,
    "200", { "pong", NULL } },
  { "control-plain-pipelined",
    { "GET /ping HTTP/1.1\r\nHost: t\r\n\r\n"
      "GET /ping HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", NULL }, 0,
    "200,200", { "pong", NULL } },
};

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    http_set_limits(0, 4096, 2);
    http_GET("/ping", ping);
    http_POST("/echo", echo);
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_lfd, 4) < 0) {
        printf("BAD listen\n"); return 1;
    }
    socklen_t al = sizeof a; getsockname(g_lfd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    int bad = 0;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        Client cl; memset(&cl, 0, sizeof cl); cl.c = &CASES[i];
        pthread_t t; pthread_create(&t, NULL, client, &cl);
        int fd = accept(g_lfd, NULL, NULL);
        handle_connection(fd);
        pthread_join(t, NULL);
        char st[128]; statuses(cl.out, st, sizeof st);
        int ok = strcmp(st, CASES[i].want_status) == 0;
        for (int j = 0; j < 2 && CASES[i].want_text[j]; j++)
            if (!strstr(cl.out, CASES[i].want_text[j])) ok = 0;
        printf("%s %s: statuses \"%s\" (want \"%s\")\n", ok ? "OK" : "BAD",
               CASES[i].name, st, CASES[i].want_status);
        if (!ok) bad++;
    }
    return bad ? 1 : 0;
}
EOF
if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DTK_HAVE_OPENSSL \
        -Wno-deprecated-declarations -I"${OSSL_INC}" -iquote "${S}" -I"${S}" \
        -o "${WORK}/h" "${WORK}/h.c" "${S}/http2.c" "${S}/encoding.c" "${S}/str.c" \
        "${S}/log.c" "${S}/capabilities.c" "-L${OSSL_LIB}" -lssl -lcrypto -lz -lpthread \
        >"${WORK}/cc.log" 2>&1; then
    fail "h2c: build: $(grep -a -m3 error "${WORK}/cc.log" | head -c 400)"
else
    "${WORK}/h" >"${WORK}/out" 2>"${WORK}/err"; rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/out"; then
        fail "h2c: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err" | head -c 300)"
    fi
fi
fi

echo ""
echo "C490: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
