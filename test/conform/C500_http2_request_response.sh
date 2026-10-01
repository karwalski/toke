#!/usr/bin/env bash
# C500_http2_request_response.sh — the HTTP/2 server answers a request whose
# headers span CONTINUATION frames, hands the handler the request body,
# sends the same response headers HTTP/1.1 over TLS sends, and closes an
# idle connection (story 121.46 items 1-4).
#
# THE DEFECTS, measured on main (e1cef7da) with this harness:
#
#   (1) CONTINUATION: the header block was appended and never decoded
#       ("only single-frame headers") -- the request was never answered.
#   (2) Bodies: DATA frames were counted for flow control and dropped;
#       req.body was always "", and a request with a body was dispatched
#       when its HEADERS frame arrived, before any DATA.
#   (3) Response headers: every response carried :status, content-length and
#       content-type only -- no X-Content-Type-Options, X-Frame-Options,
#       Referrer-Policy or Strict-Transport-Security, and none of the
#       headers the handler set.
#   (4) Idle: after 30 s without a frame the server sent a PING and treated
#       the ACK as activity, so a peer that answered PINGs (every HTTP/2
#       client does) held the pre-fork worker for as long as it stayed
#       connected -- and a peer that sent its own PINGs never even got that
#       far.
#
# NOW: header blocks are decoded when END_HEADERS arrives, on HEADERS or on
# CONTINUATION, and a frame of any other kind (or for another stream) in the
# middle of a header block is a connection error (RFC 9113 6.10).  A request
# whose HEADERS frame lacks END_STREAM is dispatched when its body is whole
# (DATA with END_STREAM, or trailers), with the body in req.body; a body over
# max_body is answered 413 and the stream reset (RFC 9113 8.1); the header
# block must arrive within timeout_secs of its first frame, and the body on
# HTTP/1.1's clock (timeout_secs + 1 s per KiB announced).  Responses carry the security headers, HSTS, CORS when
# configured and the handler's own headers (lower-cased; connection-specific
# fields dropped, RFC 9113 8.2.2), split into CONTINUATION frames when the
# block is larger than the peer's frame size.  A connection with no request in
# progress for H2_IDLE_TIMEOUT_S is closed with GOAWAY(NO_ERROR); PING,
# SETTINGS and WINDOW_UPDATE frames do not count as activity.
#
# WHAT THIS DRIVES.  A C harness that #includes src/stdlib/http.c (built with
# -DH2_IDLE_TIMEOUT_S=3), run under ASan+UBSan, over a real loopback TLS
# connection (ALPN h2) into handle_tls_connection().  Needs OpenSSL headers
# and the openssl CLI; without them the script prints a named SKIP line.
#
# Negative control: C500_STDLIB=<dir> points the harness at another stdlib
# tree.  A copy of src/stdlib/ with main's http.c/http2.c/http2.h fails every
# case except control-get.
#
# Story: 121.46

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C500_STDLIB:-${REPO_ROOT}/src/stdlib}"
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c http)" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c500_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C500: HTTP/2 CONTINUATION, request bodies, response headers, idle close"
echo "-----------------------------------------------------------------------"

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
if [ -z "${OSSL_INC}" ] || ! command -v openssl >/dev/null 2>&1; then
    echo "  SKIP: HTTP/2 needs TLS here -- no OpenSSL headers or openssl CLI on this host"
    echo ""
    echo "C500: 0 passed, 0 failed (skipped)"
    exit 0
fi
OSSL_LIB="$(dirname "${OSSL_INC}")/lib"

cat > "${WORK}/h.c" <<'EOF'
#include "http.c"
#include <pthread.h>
#include <sys/time.h>
#include <poll.h>

static Res ping(Req r) { (void)r; return make_res(200, "pong"); }
static char g_echo[8192];
static Res echo(Req r) {
    snprintf(g_echo, sizeof g_echo, "echo[%zu]:%s", strlen(r.body), r.body);
    return make_res(200, g_echo);
}
static StrPair g_hdrs[4];
static Res custom(Req r) {
    (void)r;
    Res res = make_res(201, "made");
    g_hdrs[0].key = "X-Custom";     g_hdrs[0].val = "yes-custom";
    g_hdrs[1].key = "Content-Type"; g_hdrs[1].val = "application/json";
    g_hdrs[2].key = "Connection";   g_hdrs[2].val = "close";
    res.headers.data = g_hdrs; res.headers.len = 3;
    return res;
}
static char g_ck[512];
static Res cookies(Req r) {
    const char *ck = "-", *host = "-";
    for (uint64_t i = 0; i < r.headers.len; i++) {
        if (strcmp(r.headers.data[i].key, "cookie") == 0) ck = r.headers.data[i].val;
        if (strcmp(r.headers.data[i].key, "host") == 0) host = r.headers.data[i].val;
    }
    snprintf(g_ck, sizeof g_ck, "cookie=[%s] host=[%s]", ck, host);
    return make_res(200, g_ck);
}
static char g_big[40000];
static StrPair g_bighdr[1];
static Res bighdr(Req r) {
    (void)r;
    Res res = make_res(200, "bighdr-ok");
    memset(g_big, 'v', sizeof g_big - 1); g_big[sizeof g_big - 1] = 0;
    g_bighdr[0].key = "X-Big"; g_bighdr[0].val = g_big;
    res.headers.data = g_bighdr; res.headers.len = 1;
    return res;
}

#define MAXSEG 64
typedef struct {
    const char    *name;
    unsigned char *seg[MAXSEG];
    size_t         len[MAXSEG];
    int            nseg;
    int            keepalive;    /* stay connected: answer and send PINGs */
    const char    *want[6];      /* substrings the client must receive       */
    const char    *wantnot[4];   /* substrings it must not receive           */
    int            want_goaway;  /* 1: a GOAWAY frame must arrive            */
    int            want_code;    /* GOAWAY error code, when want_goaway      */
    int            want_rst;     /* RST_STREAM error code required, +1 (0=none) */
    int            want_cont;    /* 1: a CONTINUATION frame must arrive, and
                                    no frame may exceed 16384 bytes          */
    int            max_ms;       /* 0 = no bound on how long the server took */
} Case;

static int g_port, g_lfd;
static SSL_CTX *g_sctx, *g_cctx;

static long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* ── frame builders ───────────────────────────────────────────────────── */
static void add(Case *c, const void *p, size_t n) {
    unsigned char *b = malloc(n); memcpy(b, p, n);
    c->seg[c->nseg] = b; c->len[c->nseg] = n; c->nseg++;
}
static size_t frame(unsigned char *o, unsigned type, unsigned flags,
                    unsigned sid, const unsigned char *pay, size_t n) {
    o[0] = (unsigned char)(n >> 16); o[1] = (unsigned char)(n >> 8);
    o[2] = (unsigned char)n; o[3] = (unsigned char)type; o[4] = (unsigned char)flags;
    o[5] = (unsigned char)(sid >> 24); o[6] = (unsigned char)(sid >> 16);
    o[7] = (unsigned char)(sid >> 8); o[8] = (unsigned char)sid;
    if (n) memcpy(o + 9, pay, n);
    return 9 + n;
}
static const unsigned char PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
/* :method GET, :scheme https, :path /ping, :authority t */
static const unsigned char GETPING[] = { 0x82, 0x87, 0x44, 5, '/','p','i','n','g', 0x41, 1, 't' };
/* :method POST, :scheme https, :path /echo, :authority t */
static const unsigned char POSTECHO[] = { 0x83, 0x87, 0x44, 5, '/','e','c','h','o', 0x41, 1, 't' };
/* the same with content-length: 10 (static name index 28, literal value) */
static const unsigned char POSTECHO_CL10[] = { 0x83, 0x87, 0x44, 5, '/','e','c','h','o', 0x41, 1, 't',
                                               0x5c, 2, '1', '0' };
static const unsigned char GETCUSTOM[] = { 0x82, 0x87, 0x44, 7, '/','c','u','s','t','o','m', 0x41, 1, 't' };
static const unsigned char GETBIG[] = { 0x82, 0x87, 0x44, 4, '/','b','i','g', 0x41, 1, 't' };
/* GET /ck with :authority t and two cookie fields (static name index 32) */
static const unsigned char GETCK[] = { 0x82, 0x87, 0x44, 3, '/','c','k', 0x41, 1, 't',
                                       0x60, 3, 'a','=','1', 0x60, 3, 'b','=','2' };
static size_t hello(unsigned char *o) {
    memcpy(o, PREFACE, 24);
    return 24 + frame(o + 24, 4, 0, 0, NULL, 0);
}

/* ── client ───────────────────────────────────────────────────────────── */
typedef struct { const Case *c; unsigned char out[1 << 17]; size_t n; int goaway_code;
                 int saw_cont; size_t max_frame; int rst_code; } Client;

static void *client(void *arg) {
    Client *cl = arg;
    cl->goaway_code = -1; cl->rst_code = -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return NULL; }
    SSL *ssl = SSL_new(g_cctx); SSL_set_fd(ssl, s);
    SSL_set_alpn_protos(ssl, (const unsigned char *)"\x02h2", 3);
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); close(s); return NULL; }
    for (int i = 0; i < cl->c->nseg; i++)
        if (SSL_write(ssl, cl->c->seg[i], (int)cl->c->len[i]) <= 0) break;
    /* Read until a GOAWAY, EOF or 14 s; without keepalive, also until a
     * response ends (DATA or HEADERS with END_STREAM).  With keepalive,
     * answer every PING and send one of our own every second. */
    long t0 = now_ms(), last_ping = t0;
    size_t scan = 0;
    for (;;) {
        long now = now_ms();
        if (now - t0 > 14000) break;
        if (cl->c->keepalive && now - last_ping >= 1000) {
            unsigned char f[17], z[8] = {1,2,3,4,5,6,7,8};
            frame(f, 6, 0, 0, z, 8); SSL_write(ssl, f, 17); last_ping = now;
        }
        if (!SSL_pending(ssl)) {
            struct pollfd p = { s, POLLIN, 0 };
            if (poll(&p, 1, 200) == 0) continue;
        }
        size_t room = sizeof cl->out - cl->n;
        if (!room) break;
        int r = SSL_read(ssl, cl->out + cl->n, (int)room);
        if (r <= 0) break;
        cl->n += (size_t)r;
        int done = 0;
        while (scan + 9 <= cl->n) {
            unsigned char *f = cl->out + scan;
            size_t L = ((size_t)f[0] << 16) | ((size_t)f[1] << 8) | f[2];
            if (scan + 9 + L > cl->n) break;
            if (f[3] == 9) cl->saw_cont = 1;
            if (f[3] == 3 && L == 4) cl->rst_code = (f[9] << 24) | (f[10] << 16) | (f[11] << 8) | f[12];
            if (L > cl->max_frame) cl->max_frame = L;
            if (f[3] == 7) {
                done = 1;
                if (L >= 8) cl->goaway_code = (f[13] << 24) | (f[14] << 16) | (f[15] << 8) | f[16];
            }
            if (f[3] == 6 && !(f[4] & 1)) {                   /* PING -> ACK */
                unsigned char ack[17]; frame(ack, 6, 1, 0, f + 9, 8);
                SSL_write(ssl, ack, 17);
            }
            if (!cl->c->keepalive && (f[3] == 0 || f[3] == 1) && (f[4] & 1)) done = 1;
            scan += 9 + L;
        }
        if (done) break;
    }
    SSL_free(ssl);
    close(s);
    return NULL;
}

static int contains(const Client *cl, const char *s) {
    size_t k = strlen(s);
    for (size_t i = 0; i + k <= cl->n; i++)
        if (memcmp(cl->out + i, s, k) == 0) return 1;
    return 0;
}

static int run(const Case *c) {
    static Client cl; memset(&cl, 0, sizeof cl); cl.c = c;
    pthread_t t; pthread_create(&t, NULL, client, &cl);
    int fd = accept(g_lfd, NULL, NULL);
    long t0 = now_ms();
    handle_tls_connection(fd, g_sctx);
    long took = now_ms() - t0;
    pthread_join(t, NULL);
    char why[512] = "";
    for (int i = 0; i < 6 && c->want[i]; i++)
        if (!contains(&cl, c->want[i]) && strlen(why) < 400)
            snprintf(why + strlen(why), sizeof why - strlen(why), " missing '%.40s'", c->want[i]);
    for (int i = 0; i < 4 && c->wantnot[i]; i++)
        if (contains(&cl, c->wantnot[i]) && strlen(why) < 400)
            snprintf(why + strlen(why), sizeof why - strlen(why), " unwanted '%s'", c->wantnot[i]);
    if (c->want_goaway && cl.goaway_code != c->want_code)
        snprintf(why + strlen(why), sizeof why - strlen(why),
                 " GOAWAY code %d (want %d)", cl.goaway_code, c->want_code);
    if (c->want_rst && cl.rst_code != c->want_rst - 1)
        snprintf(why + strlen(why), sizeof why - strlen(why),
                 " RST_STREAM code %d (want %d)", cl.rst_code, c->want_rst - 1);
    if (c->want_cont && (!cl.saw_cont || cl.max_frame > 16384))
        snprintf(why + strlen(why), sizeof why - strlen(why),
                 " no CONTINUATION or a frame of %zu bytes", cl.max_frame);
    if (c->max_ms && took > c->max_ms)
        snprintf(why + strlen(why), sizeof why - strlen(why),
                 " held the worker %ld ms (bound %d)", took, c->max_ms);
    int ok = why[0] == 0;
    printf("%s %s: %s (%zu bytes back, server held the worker %ld ms)\n",
           ok ? "OK" : "BAD", c->name, ok ? "as required" : why, cl.n, took);
    return ok;
}

static int alpn_h2(SSL *s, const unsigned char **out, unsigned char *outlen,
                   const unsigned char *in, unsigned int inlen, void *arg) {
    (void)s; (void)arg;
    for (unsigned i = 0; i + 1 < inlen; i += 1u + in[i])
        if (in[i] == 2 && memcmp(in + i + 1, "h2", 2) == 0) {
            *out = in + i + 1; *outlen = 2; return SSL_TLSEXT_ERR_OK;
        }
    return SSL_TLSEXT_ERR_NOACK;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) return 2;
    g_sctx = SSL_CTX_new(TLS_server_method());
    if (SSL_CTX_use_certificate_file(g_sctx, argv[1], SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(g_sctx, argv[2], SSL_FILETYPE_PEM) != 1) {
        printf("BAD tls setup\n"); return 1;
    }
    SSL_CTX_set_alpn_select_cb(g_sctx, alpn_h2, NULL);
    g_cctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(g_cctx, SSL_VERIFY_NONE, NULL);

    http_set_limits(0, 4096, 2);          /* max_body 4096, timeout_secs 2 */
    http_GET("/ping", ping);
    http_POST("/echo", echo);
    http_GET("/custom", custom);
    http_GET("/big", bighdr);
    http_GET("/ck", cookies);
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_lfd, 4) < 0) {
        printf("BAD listen\n"); return 1;
    }
    socklen_t al = sizeof a; getsockname(g_lfd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);

    static unsigned char b[1 << 15];
    size_t n;
    static Case C[16];
    int k = 0;

    /* control: a single-frame GET is served (it was on main too) */
    C[k].name = "control-get"; C[k].want[0] = "pong";
    n = hello(b); n += frame(b + n, 1, 0x5, 1, GETPING, sizeof GETPING); add(&C[k], b, n); k++;

    /* (1) the header block split across HEADERS + two CONTINUATION frames */
    C[k].name = "continuation-get"; C[k].want[0] = "pong"; C[k].max_ms = 3000;
    n = hello(b);
    n += frame(b + n, 1, 0x1 /* END_STREAM */, 1, GETPING, 3);
    n += frame(b + n, 9, 0, 1, GETPING + 3, 4);
    n += frame(b + n, 9, 0x4 /* END_HEADERS */, 1, GETPING + 7, sizeof GETPING - 7);
    add(&C[k], b, n); k++;

    /* (1) a frame of another kind in the middle of a header block is a
     *     connection error (RFC 9113 6.10) -- not "wait and see" */
    C[k].name = "ping-inside-header-block"; C[k].want_goaway = 1;
    C[k].want_code = 1 /* PROTOCOL_ERROR */; C[k].wantnot[0] = "pong"; C[k].max_ms = 3000;
    n = hello(b);
    n += frame(b + n, 1, 0x1, 1, GETPING, 3);
    { unsigned char z[8] = {0}; n += frame(b + n, 6, 0, 0, z, 8); }
    n += frame(b + n, 9, 0x4, 1, GETPING + 3, sizeof GETPING - 3);
    add(&C[k], b, n); k++;

    /* (2) a POST body in one DATA frame */
    C[k].name = "post-body"; C[k].want[0] = "echo[10]:hello-body"; C[k].max_ms = 3000;
    n = hello(b);
    n += frame(b + n, 1, 0x4 /* END_HEADERS only */, 1, POSTECHO, sizeof POSTECHO);
    n += frame(b + n, 0, 0x1, 1, (const unsigned char *)"hello-body", 10);
    add(&C[k], b, n); k++;

    /* (2) a body in three DATA frames, one padded, headers in CONTINUATION */
    C[k].name = "post-body-split-padded"; C[k].want[0] = "echo[9]:abcdefghi"; C[k].max_ms = 3000;
    n = hello(b);
    n += frame(b + n, 1, 0, 1, POSTECHO, 5);
    n += frame(b + n, 9, 0x4, 1, POSTECHO + 5, sizeof POSTECHO - 5);
    n += frame(b + n, 0, 0, 1, (const unsigned char *)"abc", 3);
    { unsigned char p[8] = { 4, 'd','e','f', 0,0,0,0 };
      n += frame(b + n, 0, 0x8 /* PADDED */, 1, p, 8); }
    n += frame(b + n, 0, 0x1, 1, (const unsigned char *)"ghi", 3);
    add(&C[k], b, n); k++;

    /* (2) a body over max_body (4096) is answered 413, not buffered */
    C[k].name = "post-body-too-large"; C[k].want[0] = "Payload Too Large";
    C[k].wantnot[0] = "echo["; C[k].max_ms = 3000;
    n = hello(b);
    n += frame(b + n, 1, 0x4, 1, POSTECHO, sizeof POSTECHO);
    { static unsigned char big[3000]; memset(big, 'x', sizeof big);
      n += frame(b + n, 0, 0, 1, big, sizeof big);
      n += frame(b + n, 0, 0x1, 1, big, sizeof big); }
    add(&C[k], b, n); k++;

    /* (2) a body that never finishes is bounded by HTTP/1.1's body clock:
     *     timeout_secs (2 s) + 1 s per KiB announced (10 bytes: + 0 s) */
    C[k].name = "post-body-stalls"; C[k].wantnot[0] = "echo["; C[k].max_ms = 3500;
    n = hello(b);
    n += frame(b + n, 1, 0x4, 1, POSTECHO_CL10, sizeof POSTECHO_CL10);
    n += frame(b + n, 0, 0, 1, (const unsigned char *)"part", 4);
    add(&C[k], b, n); C[k].keepalive = 1; k++;

    /* (2) a body shorter than its content-length is malformed (RFC 9113
     *     8.1.1): RST_STREAM(PROTOCOL_ERROR), not dispatched */
    C[k].name = "post-body-content-length-mismatch"; C[k].wantnot[0] = "echo[";
    C[k].want_rst = 1 + 1 /* PROTOCOL_ERROR */;
    n = hello(b);
    n += frame(b + n, 1, 0x4, 1, POSTECHO_CL10, sizeof POSTECHO_CL10);
    n += frame(b + n, 0, 0x1, 1, (const unsigned char *)"short", 5);
    { unsigned char z[8] = {0}; n += frame(b + n, 6, 0, 0, z, 8); }
    add(&C[k], b, n); C[k].keepalive = 1; k++;

    /* (2) the handler sees the request as HTTP/1.1 would: cookie crumbs
     *     rejoined with "; " (RFC 9113 8.2.3), :authority as host */
    C[k].name = "cookie-crumbs-and-authority"; C[k].max_ms = 3000;
    C[k].want[0] = "cookie=[a=1; b=2] host=[t]";
    n = hello(b); n += frame(b + n, 1, 0x5, 1, GETCK, sizeof GETCK); add(&C[k], b, n); k++;

    /* (3) the handler's headers (lower-cased, connection-specific ones
     *     dropped) and the security headers + HSTS */
    C[k].name = "response-headers"; C[k].max_ms = 3000;
    C[k].want[0] = "made"; C[k].want[1] = "x-custom"; C[k].want[2] = "yes-custom";
    C[k].want[3] = "application/json"; C[k].want[4] = "nosniff";
    C[k].want[5] = "max-age=63072000";
    C[k].wantnot[0] = "X-Custom"; C[k].wantnot[1] = "connection";
    C[k].wantnot[2] = "text/html";
    n = hello(b); n += frame(b + n, 1, 0x5, 1, GETCUSTOM, sizeof GETCUSTOM); add(&C[k], b, n); k++;

    /* (3) the other two security headers */
    C[k].name = "response-security-headers"; C[k].max_ms = 3000;
    C[k].want[0] = "pong"; C[k].want[1] = "x-frame-options"; C[k].want[2] = "DENY";
    C[k].want[3] = "referrer-policy"; C[k].want[4] = "strict-origin-when-cross-origin";
    C[k].want[5] = "includeSubDomains";   /* the HSTS name is HPACK static index 56 */
    n = hello(b); n += frame(b + n, 1, 0x5, 1, GETPING, sizeof GETPING); add(&C[k], b, n); k++;

    /* (3) a response header block larger than the peer's 16384-byte frame
     *     size goes out as HEADERS + CONTINUATION */
    C[k].name = "response-header-block-continued"; C[k].max_ms = 3000;
    C[k].want[0] = "bighdr-ok"; C[k].want[1] = "x-big"; C[k].want_cont = 1;
    n = hello(b); n += frame(b + n, 1, 0x5, 1, GETBIG, sizeof GETBIG); add(&C[k], b, n); k++;

    /* (4) a peer that answers our PINGs and sends its own every second,
     *     after one request, is closed with GOAWAY(NO_ERROR) once
     *     H2_IDLE_TIMEOUT_S (3 s in this build) passes with no request */
    C[k].name = "idle-pinging-peer-closed"; C[k].keepalive = 1;
    C[k].want[0] = "pong"; C[k].want_goaway = 1; C[k].want_code = 0; C[k].max_ms = 5500;
    n = hello(b); n += frame(b + n, 1, 0x5, 1, GETPING, sizeof GETPING); add(&C[k], b, n); k++;

    const char *only = getenv("C500_ONLY");
    int bad = 0;
    for (int i = 0; i < k; i++) {
        if (only && !strstr(C[i].name, only)) continue;
        if (!run(&C[i])) bad++;
    }
    return bad ? 1 : 0;
}
EOF

if ! openssl req -x509 -newkey rsa:2048 -keyout "${WORK}/key.pem" \
        -out "${WORK}/cert.pem" -days 2 -nodes -subj "/CN=localhost" >/dev/null 2>&1; then
    fail "could not generate a self-signed certificate"
elif ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DTK_HAVE_OPENSSL -DH2_IDLE_TIMEOUT_S=3 \
        -Wno-deprecated-declarations -Wno-macro-redefined -I"${OSSL_INC}" -iquote "${S}" -I"${S}" \
        -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" "-L${OSSL_LIB}" -lssl -lcrypto -lz -lpthread \
        >"${WORK}/cc.log" 2>&1; then
    fail "build: $(grep -a -m3 error "${WORK}/cc.log" | head -c 400)"
else
    "${WORK}/h" "${WORK}/cert.pem" "${WORK}/key.pem" >"${WORK}/out" 2>"${WORK}/err"; rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/out"; then
        fail "exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err" | head -c 300)"
    fi
fi

echo ""
echo "C500: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
