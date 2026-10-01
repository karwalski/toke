#!/usr/bin/env bash
# C662_http_conn_buffer_and_oom.sh -- the HTTP/1.1 connection buffer grows on
# demand instead of being allocated at max_header + max_body, and a request
# whose strings cannot be allocated is answered 500 instead of reaching the
# router half-built (story 121.29: audit-120 REL-02, REL-08).
#
# THE DEFECTS, measured on main (437dc471) with this harness:
#   REL-02  handle_connection / handle_tls_connection malloc()ed
#           max_header + max_body + slack (1,057,288 bytes with the defaults)
#           for every connection before reading a byte, so N idle keep-alive
#           connections held N MB.  The harness sees that as the largest single
#           allocation made while serving one 40-byte GET.
#   REL-08  parse_request() did not check one of its strdup()s.  Failing any
#           of them (method, path, a header name or value, the body) sent the
#           request on with a NULL in it: a NULL method answered 405, a NULL
#           path 404, and a NULL header name crashed the server (ASan SEGV in
#           the dispatcher's header scan, strdup 3 of 9).
#
# NOW: the buffer starts at 16 KiB (or the frame cap, if smaller), doubles up
# to the same max_header + max_body + slack bound as it fills, and shrinks back
# for the next request on the connection; parse_request() checks every
# allocation, frees what it had, and the connection answers 500 and closes.
#
# WHAT THIS DRIVES.  A C harness that #includes src/stdlib/http.c with malloc,
# realloc and strdup wrapped (to record the largest allocation, and to fail the
# k-th strdup), built under ASan+UBSan, serving real loopback connections with
# handle_connection().  Cases:
#   small GET: largest allocation < 64 KiB; 200;
#   600 KB Content-Length body, 600 KB chunked body: echoed length exact;
#   1 MiB + 1 body: 413; a big POST then a small GET on one keep-alive
#   connection: both answered (the buffer grew, then shrank);
#   failing strdup number k, for every k that parse_request makes: 500, the
#   handler never runs, no sanitizer report.
#
# Negative control: C662_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib the "small GET" allocation case fails
# (1,057,288 bytes), strdups 1 and 2 answer 405 and 404, and strdup 3 crashes
# the harness.
#
# Story: 121.29
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
S="${C662_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c662_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C662: HTTP/1.1 connection buffer grows on demand; OOM in parsing is a 500 (121.29)"
echo "-----------------------------------------------------------------------------------"

cat > "${WORK}/h.c" <<'EOF'
#include <stdlib.h>
#include <string.h>
static size_t g_max_alloc;
static int    g_fail_at, g_sd_calls, g_handler_runs;
static void *t_malloc(size_t n) { if (n > g_max_alloc) g_max_alloc = n; return malloc(n); }
static void *t_realloc(void *p, size_t n) { if (n > g_max_alloc) g_max_alloc = n; return realloc(p, n); }
static char *t_strdup(const char *s) {
    g_sd_calls++;
    if (g_fail_at && g_sd_calls == g_fail_at) return NULL;
    return strdup(s);
}
#define malloc  t_malloc
#define realloc t_realloc
#define strdup  t_strdup
#include "http.c"
#undef malloc
#undef realloc
#undef strdup
#include <pthread.h>

static char g_out[256];
static int  g_sd_at_handler;
static Res echo(Req r) {
    g_handler_runs++;
    g_sd_at_handler = g_sd_calls;
    const char *xa = "-";
    for (uint64_t i = 0; i < r.headers.len; i++)
        if (strcasecmp(r.headers.data[i].key, "X-A") == 0) xa = r.headers.data[i].val;
    snprintf(g_out, sizeof g_out, "len=%zu xa=%s", strlen(r.body ? r.body : ""), xa);
    return make_res(200, g_out);
}

static int g_lfd, g_port;
typedef struct { const char *req; size_t len; char *out; size_t cap; size_t n; } Cl;

static void *client(void *arg) {
    Cl *c = arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return NULL; }
    struct timeval tv = { 10, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    size_t off = 0;
    while (off < c->len) {
        ssize_t w = send(s, c->req + off, c->len - off, 0);
        if (w <= 0) break;
        off += (size_t)w;
    }
    shutdown(s, SHUT_WR);
    for (;;) {
        ssize_t r = recv(s, c->out + c->n, c->cap - 1 - c->n, 0);
        if (r <= 0) break;
        c->n += (size_t)r;
        if (c->n >= c->cap - 1) break;
    }
    c->out[c->n] = '\0';
    close(s);
    return NULL;
}

/* serve one connection carrying req; returns the raw response text */
static char *run(const char *req, size_t len) {
    static char out[65536];
    Cl c = { req, len, out, sizeof out, 0 };
    pthread_t t; pthread_create(&t, NULL, client, &c);
    int fd = accept(g_lfd, NULL, NULL);
    if (fd >= 0) handle_connection(fd);
    pthread_join(t, NULL);
    return out;
}

static int bad;
static void check(int ok, const char *name, const char *got) {
    if (ok) printf("OK %s\n", name);
    else { printf("BAD %s -- got: %.200s\n", name, got); bad++; }
}
static int count(const char *h, const char *n) {
    int k = 0; for (const char *p = h; (p = strstr(p, n)); p++) k++; return k;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);   /* results survive a crash */
    http_GET("/x", echo);
    http_POST("/x", echo);
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(g_lfd, (struct sockaddr *)&a, sizeof a); listen(g_lfd, 8);
    socklen_t al = sizeof a; getsockname(g_lfd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);

    char msg[256];
    /* REL-02: a small request allocates a small buffer */
    const char *small = "GET /x HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n";
    g_max_alloc = 0;
    char *o = run(small, strlen(small));
    snprintf(msg, sizeof msg, "small GET: largest allocation %zu bytes (< 65536)", g_max_alloc);
    check(g_max_alloc < 65536 && strstr(o, " 200 ") != NULL, msg, o);

    /* bodies that need the buffer to grow */
    size_t big = 600000;
    char *body = malloc(big + 1); memset(body, 'b', big); body[big] = 0;
    char *req = malloc(big * 2 + 4096);
    int hl = sprintf(req, "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", big);
    memcpy(req + hl, body, big);
    o = run(req, (size_t)hl + big);
    check(strstr(o, "len=600000") != NULL, "600 KB Content-Length body echoed whole", o);

    size_t n = (size_t)sprintf(req, "POST /x HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
    for (size_t left = big; left; ) {
        size_t cs = left < 7000 ? left : 7000;
        n += (size_t)sprintf(req + n, "%zx\r\n", cs);
        memcpy(req + n, body, cs); n += cs;
        memcpy(req + n, "\r\n", 2); n += 2;
        left -= cs;
    }
    n += (size_t)sprintf(req + n, "0\r\n\r\n");
    o = run(req, n);
    check(strstr(o, "len=600000") != NULL, "600 KB chunked body decoded whole", o);

    hl = sprintf(req, "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: %u\r\n\r\n",
                 HTTP_DEFAULT_MAX_BODY_SIZE + 1u);
    o = run(req, (size_t)hl);
    check(strstr(o, " 413 ") != NULL, "max_body + 1 is still 413", o);

    /* grow, then shrink back, on one keep-alive connection */
    hl = sprintf(req, "POST /x HTTP/1.1\r\nHost: a\r\nContent-Length: %zu\r\n\r\n", big);
    memcpy(req + hl, body, big); n = (size_t)hl + big;
    n += (size_t)sprintf(req + n, "GET /x HTTP/1.1\r\nHost: a\r\nX-A: two\r\nConnection: close\r\n\r\n");
    o = run(req, n);
    check(count(o, " 200 ") == 2 && strstr(o, "len=600000") && strstr(o, "xa=two"),
          "big POST then small GET on one connection: both answered", o);

    /* REL-08: fail each strdup parse_request makes */
    const char *hreq = "GET /x HTTP/1.1\r\nHost: a\r\nX-A: v\r\nConnection: close\r\n\r\n";
    g_sd_calls = 0; g_fail_at = 0; g_handler_runs = 0;
    o = run(hreq, strlen(hreq));
    int K = g_sd_at_handler;
    snprintf(msg, sizeof msg, "baseline: 200 with the header (%d strdups before the handler)", K);
    check(strstr(o, " 200 ") && strstr(o, "xa=v") && K >= 8, msg, o);
    for (int k = 1; k <= K; k++) {
        g_sd_calls = 0; g_fail_at = k; g_handler_runs = 0;
        o = run(hreq, strlen(hreq));
        snprintf(msg, sizeof msg, "strdup %d of %d fails: 500, handler not run", k, K);
        check(strstr(o, " 500 ") != NULL && g_handler_runs == 0, msg, o);
    }
    g_fail_at = 0;
    return bad ? 1 : 0;
}
EOF

# 127.268: the rest of std.http's link set (h.c #includes http.c itself)
# comes from tkc's dependency table, not a hand list.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c http 2> "${WORK}/cc.log")" \
    || { fail "harness link set: $(head -c 400 "${WORK}/cc.log")"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! ${CC} -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" \
        -I"${REPO_ROOT}/stdlib/vendor/tomlc99" -Wno-unused-function \
        -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" \
        -lz -lpthread -lm > "${WORK}/cc.log" 2>&1; then
    fail "harness build: $(grep -a -m3 -E 'error' "${WORK}/cc.log" | head -c 400)"
    echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1
fi
"${WORK}/h" > "${WORK}/out" 2> "${WORK}/err"; rc=$?
while IFS= read -r line; do
    case "${line}" in
        OK\ *)  pass "${line#OK }" ;;
        BAD\ *) fail "${line#BAD }" ;;
    esac
done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
if [ "${rc}" -ne 0 ] && { ! grep -a -q '^BAD ' "${WORK}/out" ||
        grep -a -q -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err"; }; then
    fail "harness exit ${rc} after $(grep -a -c -E '^(OK|BAD) ' "${WORK}/out") cases -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err" | head -c 200)"
fi
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
