#!/usr/bin/env bash
# C721_router_wire_headers.sh -- std.router over the wire: request headers
# reach the middleware, response headers reach the client, every status
# carries its own reason phrase, and no header can split the response
# (story 121.90).
#
# THE DEFECT, measured on main (e9ec45b5):
#   - router_handle_fd and router_serve called router_dispatch with NO
#     request headers.  So over a socket the gzip middleware never saw
#     Accept-Encoding, the ETag middleware never saw If-None-Match, CORS never
#     saw Origin, and a router_static route never saw If-None-Match or Range
#     (its trampolines passed NULL for both).
#   - router_send_response wrote Content-Type, Content-Length and Connection
#     and nothing else: header_names/header_values (Content-Encoding, ETag,
#     Cache-Control, Content-Range, every CORS header) were dropped.  And the
#     status line said "OK" for every status: "HTTP/1.1 404 OK",
#     "HTTP/1.1 204 OK".
#
# THE FIX.  Both entry points parse the request's header fields and dispatch
# with them (router_dispatch_ex).  The writer sends the reason phrase of the
# status (an unregistered code an empty one, RFC 9112 sec. 4; a status
# outside 100..599 as 500) and the response's headers.  The writer owns
# framing: a Content-Length, Transfer-Encoding or Connection entry is not
# sent, and Content-Type is content_type.  A header whose name is not a token
# or whose value holds CR, LF or another control byte is DROPPED (response
# splitting); such a content_type becomes application/octet-stream.  1xx, 204
# and 304 carry no body and no Content-Length.
#
# WHAT IS ASSERTED, by a C harness under ASan/UBSan, each request written to
# a socketpair and served by router_handle_fd in a forked child:
#   reason phrases (200, 201, 404, an unregistered 299, an invalid 0 -> 500);
#   request headers reach a handler (case-insensitive name, trimmed value);
#   gzip: Accept-Encoding: gzip -> Content-Encoding: gzip + Vary, the body
#   inflates whole and Content-Length counts it; no Accept-Encoding -> plain;
#   CORS: an allowed Origin gets Access-Control-Allow-Origin + Vary: Origin; a
#   preflight is "204 No Content" with Allow-Methods and no Content-Length; a
#   foreign Origin gets no CORS header;
#   ETag middleware: ETag sent; If-None-Match -> "304 Not Modified", no body;
#   router_static: ETag/Cache-Control/Accept-Ranges sent, ONE Content-Type;
#   If-None-Match -> 304; Range -> "206 Partial Content" + Content-Range and
#   the bytes; an unsatisfiable Range -> 416 with Content-Range: bytes */N;
#   injection: CR/LF in a header value, a header name or content_type sends
#   no forged line; a handler's Content-Length/Connection/Transfer-Encoding
#   are not sent; the well-formed header next to them is.
#
# Negative control: C721_STDLIB=<dir> builds the harness against another
# stdlib tree.  Against main's src/stdlib (e9ec45b5) it is 9 passed,
# 26 failed.  What passes there passes because main sent no response
# headers at all: the 200 status line, a plain body with no Accept-Encoding,
# Content-Length equal to the (uncompressed) body, no CORS header for a
# foreign Origin, one Content-Type, and the absence checks of the injection
# and framing cases.
#
# Story: 121.90
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C721_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
echo "C721: std.router sends request headers in and response headers out (121.90)"
echo "----------------------------------------------------------------------"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c721_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

mkdir -p "${WORK}/pub"
printf 'static file body\n' > "${WORK}/pub/a.txt"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

cat > "${WORK}/h.c" <<'EOF'
#include "router.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <zlib.h>

static int bad;
static void report(int ok, const char *fmt, ...)
{
    va_list ap;
    printf("%s ", ok ? "OK" : "BAD");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    if (!ok) bad++;
}

/* One exchange: the header block (NUL-terminated, ending at its last CRLF
 * before the blank line) and the body bytes after it. */
typedef struct { char head[16384]; unsigned char body[65536]; size_t blen; int ok; } Ex;

static Ex ex;
static void rt(TkRouter *r, const char *req)
{
    memset(&ex, 0, sizeof ex);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return;
    if (write(sv[0], req, strlen(req)) != (ssize_t)strlen(req)) return;
    shutdown(sv[0], SHUT_WR);
    pid_t pid = fork();
    if (pid == 0) { close(sv[0]); router_handle_fd(r, sv[1]); _exit(0); }
    close(sv[1]);
    static unsigned char buf[1 << 17];
    size_t got = 0; ssize_t n;
    while (got < sizeof buf && (n = read(sv[0], buf + got, sizeof buf - got)) > 0) got += (size_t)n;
    close(sv[0]);
    waitpid(pid, NULL, 0);
    for (size_t i = 0; i + 3 < got; i++)
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) {
            size_t hl = i + 2 < sizeof ex.head ? i + 2 : sizeof ex.head - 1;
            memcpy(ex.head, buf, hl); ex.head[hl] = '\0';
            ex.blen = got - (i + 4);
            if (ex.blen > sizeof ex.body) ex.blen = sizeof ex.body;
            memcpy(ex.body, buf + i + 4, ex.blen);
            ex.ok = 1;
            return;
        }
}

/* the status line, up to its CRLF */
static int status_is(const char *want)
{
    size_t n = strlen(want);
    return ex.ok && strncmp(ex.head, want, n) == 0 && ex.head[n] == '\r';
}
static const char *status_line(void)
{
    static char sl[128];
    size_t i = 0;
    while (ex.head[i] && ex.head[i] != '\r' && i < sizeof sl - 1) { sl[i] = ex.head[i]; i++; }
    sl[i] = '\0';
    return ex.ok ? sl : "(no response)";
}
/* value of header `name` (first), or NULL; copies into a static buffer */
static const char *hdr(const char *name)
{
    static char v[1024];
    size_t nl = strlen(name);
    const char *p = strstr(ex.head, "\r\n");
    while (p) {
        p += 2;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') {
            const char *s = p + nl + 1;
            while (*s == ' ') s++;
            const char *e = strstr(s, "\r\n");
            size_t l = e ? (size_t)(e - s) : strlen(s);
            if (l >= sizeof v) l = sizeof v - 1;
            memcpy(v, s, l); v[l] = '\0';
            return v;
        }
        p = strstr(p, "\r\n");
    }
    return NULL;
}
static int hdr_count(const char *name)
{
    int c = 0;
    size_t nl = strlen(name);
    for (const char *p = strstr(ex.head, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (strncasecmp(p, name, nl) == 0 && p[nl] == ':') c++;
    }
    return c;
}
#define HAS(name, val) (hdr(name) && strcmp(hdr(name), (val)) == 0)
#define SHOW(name) (hdr(name) ? hdr(name) : "(none)")

static char big[4097];
static TkRouteResp h_big(TkRouteCtx c) { (void)c; return router_resp_ok(big, "text/plain; charset=utf-8"); }
static TkRouteResp h_hi(TkRouteCtx c)  { (void)c; return router_resp_ok("hi", "text/plain"); }
static TkRouteResp h_201(TkRouteCtx c) { (void)c; return router_resp_status(201, "made"); }
static TkRouteResp h_299(TkRouteCtx c) { (void)c; return router_resp_status(299, "odd"); }
static TkRouteResp h_0(TkRouteCtx c)   { (void)c; return router_resp_status(0, "zero"); }
static TkRouteResp h_echo(TkRouteCtx c)
{
    for (uint64_t i = 0; i < c.nreq_headers; i++)
        if (c.req_header_names && strcasecmp(c.req_header_names[i], "X-Echo") == 0)
            return router_resp_ok(c.req_header_values[i], "text/plain");
    return router_resp_ok("(no X-Echo)", "text/plain");
}
static const char *evil_n[] = {
    "X-Good", "X-Evil", "X-Bad\r\nSet-Cookie: n=1", "X-Lf",
    "Content-Length", "Connection", "Transfer-Encoding",
};
static const char *evil_v[] = {
    "1", "a\r\nSet-Cookie: pwn=1", "v", "a\nInjected: 1",
    "999", "keep-alive", "chunked",
};
static TkRouteResp h_evil(TkRouteCtx c)
{
    (void)c;
    TkRouteResp r = router_resp_ok("body", "text/plain\r\nX-Ct-Inject: 1");
    r.header_names = evil_n; r.header_values = evil_v; r.nheaders = 7;
    return r;
}

int main(int argc, char **argv)
{
    (void)argc;
    const char *dir = argv[1];
    for (int i = 0; i < 4096; i++) big[i] = "the quick brown fox jumps over the lazy dog\n"[i % 44];

    /* ── reason phrases ── */
    TkRouter *b = router_new();
    router_get(b, "/hi", h_hi);
    router_get(b, "/201", h_201);
    router_get(b, "/299", h_299);
    router_get(b, "/0", h_0);
    router_get(b, "/echo", h_echo);
    rt(b, "GET /hi HTTP/1.1\r\nHost: t\r\n\r\n");
    report(status_is("HTTP/1.1 200 OK"), "200: status line [%s]", status_line());
    rt(b, "GET /201 HTTP/1.1\r\nHost: t\r\n\r\n");
    report(status_is("HTTP/1.1 201 Created"), "201: status line [%s] (want 201 Created)", status_line());
    rt(b, "GET /nope HTTP/1.1\r\nHost: t\r\n\r\n");
    report(status_is("HTTP/1.1 404 Not Found"), "404: status line [%s] (want 404 Not Found)", status_line());
    rt(b, "GET /299 HTTP/1.1\r\nHost: t\r\n\r\n");
    report(status_is("HTTP/1.1 299 "), "299 (unregistered): status line [%s] (want an empty reason)", status_line());
    rt(b, "GET /0 HTTP/1.1\r\nHost: t\r\n\r\n");
    report(status_is("HTTP/1.1 500 Internal Server Error"), "status 0: status line [%s] (want 500)", status_line());

    /* ── request headers reach the handler ── */
    rt(b, "GET /echo HTTP/1.1\r\nHost: t\r\nx-echo:   hello there  \r\n\r\n");
    report(ex.blen == 11 && memcmp(ex.body, "hello there", 11) == 0,
           "request header x-echo reaches the handler, trimmed: [%.*s]", (int)ex.blen, ex.body);

    /* ── gzip middleware ── */
    TkRouter *g = router_new();
    router_get(g, "/t", h_big);
    router_use_gzip(g, 256);
    rt(g, "GET /t HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip, deflate\r\n\r\n");
    report(HAS("Content-Encoding", "gzip"), "gzip: Content-Encoding %s (want gzip)", SHOW("Content-Encoding"));
    report(HAS("Vary", "Accept-Encoding"), "gzip: Vary %s (want Accept-Encoding)", SHOW("Vary"));
    {
        const char *cl = hdr("Content-Length");
        size_t clv = cl ? (size_t)strtoull(cl, NULL, 10) : (size_t)-1;
        unsigned char out[8192];
        z_stream zs; memset(&zs, 0, sizeof zs);
        int zr = inflateInit2(&zs, 15 + 16);
        zs.next_in = ex.body; zs.avail_in = (uInt)ex.blen;
        zs.next_out = out; zs.avail_out = sizeof out;
        if (zr == Z_OK) zr = inflate(&zs, Z_FINISH);
        size_t ol = zs.total_out;
        inflateEnd(&zs);
        report(clv == ex.blen, "gzip: Content-Length %zu counts the %zu body bytes", clv, ex.blen);
        report(zr == Z_STREAM_END && ol == 4096 && memcmp(out, big, 4096) == 0,
               "gzip: the body inflates whole (%zu -> %zu bytes, want 4096)", ex.blen, ol);
    }
    rt(g, "GET /t HTTP/1.1\r\nHost: t\r\n\r\n");
    report(!hdr("Content-Encoding") && ex.blen == 4096 && memcmp(ex.body, big, 4096) == 0,
           "gzip: no Accept-Encoding -> plain body (%zu bytes, Content-Encoding %s)", ex.blen, SHOW("Content-Encoding"));

    /* ── CORS ── */
    static const char *origins[] = { "https://a.example", NULL };
    static const char *methods[] = { "GET", "POST", NULL };
    TkCorsOpts co; memset(&co, 0, sizeof co);
    co.allowed_origins = origins; co.allowed_methods = methods; co.max_age = -1;
    TkRouter *c = router_new();
    router_get(c, "/hi", h_hi);
    router_use_cors(c, co);
    rt(c, "GET /hi HTTP/1.1\r\nHost: t\r\nOrigin: https://a.example\r\n\r\n");
    report(HAS("Access-Control-Allow-Origin", "https://a.example"),
           "cors: Access-Control-Allow-Origin %s (want https://a.example)", SHOW("Access-Control-Allow-Origin"));
    report(HAS("Vary", "Origin"), "cors: Vary %s (want Origin)", SHOW("Vary"));
    rt(c, "OPTIONS /hi HTTP/1.1\r\nHost: t\r\nOrigin: https://a.example\r\n"
          "Access-Control-Request-Method: POST\r\n\r\n");
    report(status_is("HTTP/1.1 204 No Content"), "cors preflight: status line [%s] (want 204 No Content)", status_line());
    report(HAS("Access-Control-Allow-Methods", "GET, POST"),
           "cors preflight: Access-Control-Allow-Methods %s", SHOW("Access-Control-Allow-Methods"));
    report(hdr_count("Content-Length") == 0 && ex.blen == 0,
           "cors preflight: 204 has no Content-Length (%d) and no body (%zu)", hdr_count("Content-Length"), ex.blen);
    rt(c, "GET /hi HTTP/1.1\r\nHost: t\r\nOrigin: https://evil.example\r\n\r\n");
    report(!hdr("Access-Control-Allow-Origin"), "cors: a foreign Origin gets no Access-Control-Allow-Origin");

    /* ── ETag middleware ── */
    TkRouter *e = router_new();
    router_get(e, "/hi", h_hi);
    router_use_etag(e);
    rt(e, "GET /hi HTTP/1.1\r\nHost: t\r\n\r\n");
    char etag[256] = "";
    if (hdr("ETag")) snprintf(etag, sizeof etag, "%s", hdr("ETag"));
    report(etag[0] == 'W', "etag mw: ETag %s (want a weak ETag)", etag[0] ? etag : "(none)");
    {
        char req[512];
        snprintf(req, sizeof req, "GET /hi HTTP/1.1\r\nHost: t\r\nIf-None-Match: %s\r\n\r\n", etag[0] ? etag : "W/\"x\"");
        rt(e, req);
        report(status_is("HTTP/1.1 304 Not Modified"), "etag mw: If-None-Match -> [%s] (want 304 Not Modified)", status_line());
        report(ex.blen == 0 && hdr_count("Content-Length") == 0,
               "etag mw: 304 has no body (%zu) and no Content-Length (%d)", ex.blen, hdr_count("Content-Length"));
    }

    /* ── router_static ── */
    TkRouter *s = router_new();
    router_static(s, "/s", dir);
    rt(s, "GET /s/a.txt HTTP/1.1\r\nHost: t\r\n\r\n");
    char setag[256] = "";
    if (hdr("ETag")) snprintf(setag, sizeof setag, "%s", hdr("ETag"));
    report(status_is("HTTP/1.1 200 OK") && setag[0] == '"', "static: 200 with ETag %s", setag[0] ? setag : "(none)");
    report(HAS("Cache-Control", "public, max-age=604800"), "static: Cache-Control %s", SHOW("Cache-Control"));
    report(HAS("Accept-Ranges", "bytes"), "static: Accept-Ranges %s", SHOW("Accept-Ranges"));
    report(hdr_count("Content-Type") == 1, "static: exactly one Content-Type (%d)", hdr_count("Content-Type"));
    {
        char req[512];
        snprintf(req, sizeof req, "GET /s/a.txt HTTP/1.1\r\nHost: t\r\nIf-None-Match: %s\r\n\r\n", setag[0] ? setag : "\"x\"");
        rt(s, req);
        report(status_is("HTTP/1.1 304 Not Modified"), "static: If-None-Match -> [%s] (want 304 Not Modified)", status_line());
    }
    rt(s, "GET /s/a.txt HTTP/1.1\r\nHost: t\r\nRange: bytes=0-5\r\n\r\n");
    report(status_is("HTTP/1.1 206 Partial Content"), "static: Range -> [%s] (want 206 Partial Content)", status_line());
    report(HAS("Content-Range", "bytes 0-5/17"), "static: Content-Range %s (want bytes 0-5/17)", SHOW("Content-Range"));
    report(ex.blen == 6 && memcmp(ex.body, "static", 6) == 0, "static: the 6 ranged bytes [%.*s]", (int)ex.blen, ex.body);
    rt(s, "GET /s/a.txt HTTP/1.1\r\nHost: t\r\nRange: bytes=999-\r\n\r\n");
    report(status_is("HTTP/1.1 416 Range Not Satisfiable") && HAS("Content-Range", "bytes */17"),
           "static: unsatisfiable Range -> [%s] Content-Range %s", status_line(), SHOW("Content-Range"));

    /* ── header injection ── */
    TkRouter *v = router_new();
    router_get(v, "/evil", h_evil);
    rt(v, "GET /evil HTTP/1.1\r\nHost: t\r\n\r\n");
    report(HAS("X-Good", "1"), "injection: the well-formed header is sent (X-Good %s)", SHOW("X-Good"));
    report(!strcasestr(ex.head, "Set-Cookie"), "injection: CR/LF in a value or name forges no Set-Cookie line");
    report(!strcasestr(ex.head, "Injected") && !strcasestr(ex.head, "X-Evil") && !strcasestr(ex.head, "X-Lf"),
           "injection: the headers holding CR/LF are dropped whole");
    report(!strcasestr(ex.head, "X-Ct-Inject") && HAS("Content-Type", "application/octet-stream"),
           "injection: a content_type holding CR/LF forges nothing (Content-Type %s)", SHOW("Content-Type"));
    report(hdr_count("Content-Length") == 1 && HAS("Content-Length", "4") && ex.blen == 4,
           "framing: one Content-Length, the router's (%d, %s)", hdr_count("Content-Length"), SHOW("Content-Length"));
    report(hdr_count("Connection") == 1 && HAS("Connection", "close") && !hdr("Transfer-Encoding"),
           "framing: the handler's Connection and Transfer-Encoding are not sent");
    return bad ? 1 : 0;
}
EOF
if ! ASAN_OPTIONS=detect_leaks=0 "${CC}" "${CF[@]}" -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" -lz >"${WORK}/h.cc" 2>&1; then
    fail "harness build: $(grep -a -m2 error "${WORK}/h.cc" | head -c 400)"
else
    ASAN_OPTIONS=detect_leaks=0 "${WORK}/h" "${WORK}/pub" >"${WORK}/h.out" 2>&1
    rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/h.out")
    grep -a -q -E '^(OK|BAD) ' "${WORK}/h.out" || fail "harness produced nothing: $(head -c 300 "${WORK}/h.out")"
    if grep -a -q -E 'Sanitizer|runtime error' "${WORK}/h.out"; then
        fail "sanitizer report: $(grep -a -m1 -E 'Sanitizer|runtime error' "${WORK}/h.out" | head -c 300)"
    elif [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/h.out"; then
        fail "harness exited ${rc}: $(tail -3 "${WORK}/h.out" | head -c 300)"
    fi
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
