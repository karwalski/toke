#!/usr/bin/env bash
# C600_http2_flow_control.sh -- the HTTP/2 server sends response DATA within
# the peer's flow-control windows (story 121.50 item 1).
#
# THE DEFECT, measured on main (f605aec9) with this harness: h2_send_response
# wrote the whole body in max_frame_size DATA frames, ignoring both the
# stream's and the connection's send window.  A body over 64 KiB therefore
# overran any client on the default 65535-byte window -- a FLOW_CONTROL_ERROR
# in RFC 9113 6.9 terms, and nghttp2/browsers fail the stream.  A client that
# set SETTINGS_INITIAL_WINDOW_SIZE=0 still received the whole body.  And a
# WINDOW_UPDATE that pushed a window past 2^31-1 was a signed 32-bit overflow
# (UBSan aborts the worker) instead of a FLOW_CONTROL_ERROR.
#
# NOW: DATA goes out as the windows allow; what they do not take waits on the
# stream (copied) and is sent as WINDOW_UPDATE frames (or a larger
# INITIAL_WINDOW_SIZE) open the window, while the connection keeps serving
# other streams.  A peer that never opens its window is bounded by
# timeout_secs from the last byte sent.  A window pushed past 2^31-1 is
# GOAWAY(FLOW_CONTROL_ERROR) for the connection, RST_STREAM for a stream.
#
# WHAT THIS DRIVES.  A C harness that #includes src/stdlib/http.c (as C500
# does), under ASan+UBSan, over a real loopback TLS connection (ALPN h2) into
# handle_tls_connection(), with a frame-level client that decides when to
# open the window.  Needs OpenSSL headers and the openssl CLI; without them
# the script prints a named SKIP line.
#
# 121.80: the server context uses http.c's own ALPN callback (alpn_select_cb)
# and the client offers h2 and http/1.1, as a browser does; a case runs only
# when h2 was really negotiated.  Against a stdlib whose ALPN offers only
# http/1.1 (main before 121.80) every case fails "ALPN did not negotiate h2".
#
# Negative control: C600_STDLIB=<dir> points the harness at another stdlib
# tree.  A copy of src/stdlib/ with main's http.c/http2.c/http2.h fails
# default-window, initial-window-zero, multiplexed and window-overflow.
#
# Story: 121.50

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C600_STDLIB:-${REPO_ROOT}/src/stdlib}"
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c http)" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c600_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C600: HTTP/2 response DATA respects the peer's flow-control windows"
echo "-------------------------------------------------------------------"

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
if [ -z "${OSSL_INC}" ] || ! command -v openssl >/dev/null 2>&1; then
    echo "  SKIP: HTTP/2 needs TLS here -- no OpenSSL headers or openssl CLI on this host"
    echo ""
    echo "C600: 0 passed, 0 failed (skipped)"
    exit 0
fi
OSSL_LIB="$(dirname "${OSSL_INC}")/lib"

cat > "${WORK}/h.c" <<'EOF'
#include "http.c"
#include <pthread.h>
#include <sys/time.h>
#include <poll.h>

#define BIG 200000
static char g_big[BIG + 1];
static Res big(Req r) { (void)r; return make_res(200, g_big); }
static Res ping(Req r) { (void)r; return make_res(200, "pong"); }

static int g_port, g_lfd;
static SSL_CTX *g_sctx, *g_cctx;

static long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

/* ── client ───────────────────────────────────────────────────────────── */
typedef struct {
    SSL *ssl; int s;
    unsigned char in[1 << 16]; size_t n;
    size_t data[16];            /* DATA payload bytes per stream id      */
    int end[16], hdrs[16], rst[16];
    int goaway;                 /* GOAWAY error code + 1, 0 = none       */
    int eof;
    unsigned char body1[BIG];   /* stream 1's body, to check its bytes   */
    char why[512];
} Cl;

static void bad(Cl *c, const char *m) {
    if (strlen(c->why) < 400)
        snprintf(c->why + strlen(c->why), sizeof c->why - strlen(c->why), " %s", m);
}

static void frame(Cl *c, unsigned type, unsigned flags, unsigned sid,
                  const unsigned char *pay, size_t n) {
    unsigned char o[9 + 64];
    o[0] = (unsigned char)(n >> 16); o[1] = (unsigned char)(n >> 8);
    o[2] = (unsigned char)n; o[3] = (unsigned char)type; o[4] = (unsigned char)flags;
    o[5] = (unsigned char)(sid >> 24); o[6] = (unsigned char)(sid >> 16);
    o[7] = (unsigned char)(sid >> 8); o[8] = (unsigned char)sid;
    if (n) memcpy(o + 9, pay, n);
    SSL_write(c->ssl, o, (int)(9 + n));
}
static void wu(Cl *c, unsigned sid, unsigned inc) {
    unsigned char p[4] = { (unsigned char)(inc >> 24), (unsigned char)(inc >> 16),
                           (unsigned char)(inc >> 8), (unsigned char)inc };
    frame(c, 8, 0, sid, p, 4);
}
/* :method GET, :scheme https, :path /big | /ping, :authority t */
static const unsigned char GETBIG[]  = { 0x82, 0x87, 0x44, 4, '/','b','i','g', 0x41, 1, 't' };
static const unsigned char GETPING[] = { 0x82, 0x87, 0x44, 5, '/','p','i','n','g', 0x41, 1, 't' };
static void get(Cl *c, unsigned sid, int bigp) {
    if (bigp) frame(c, 1, 0x5, sid, GETBIG, sizeof GETBIG);
    else      frame(c, 1, 0x5, sid, GETPING, sizeof GETPING);
}
/* preface + SETTINGS (initial window iw, or none when iw < 0) */
static void hello(Cl *c, long iw) {
    SSL_write(c->ssl, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24);
    if (iw < 0) { frame(c, 4, 0, 0, NULL, 0); return; }
    unsigned char p[6] = { 0, 4, (unsigned char)(iw >> 24), (unsigned char)(iw >> 16),
                           (unsigned char)(iw >> 8), (unsigned char)iw };
    frame(c, 4, 0, 0, p, 6);
}

/* pump -- read and account frames until quiet_ms pass with nothing new, or
 * (end_sid) that stream ends, or GOAWAY/EOF; never longer than 12 s. */
static void pump(Cl *c, int quiet_ms, int end_sid) {
    long t0 = now_ms(), last = t0;
    for (;;) {
        long now = now_ms();
        if (now - t0 > 12000 || now - last > quiet_ms) return;
        if (!SSL_pending(c->ssl)) {
            struct pollfd p = { c->s, POLLIN, 0 };
            if (poll(&p, 1, 50) == 0) continue;
        }
        int r = SSL_read(c->ssl, c->in + c->n, (int)(sizeof c->in - c->n));
        if (r <= 0) { c->eof = 1; return; }
        c->n += (size_t)r; last = now_ms();
        size_t off = 0;
        while (off + 9 <= c->n) {
            unsigned char *f = c->in + off;
            size_t L = ((size_t)f[0] << 16) | ((size_t)f[1] << 8) | f[2];
            if (off + 9 + L > c->n) break;
            unsigned sid = ((unsigned)(f[5] & 0x7f) << 24) | ((unsigned)f[6] << 16)
                         | ((unsigned)f[7] << 8) | f[8];
            if (sid < 16) {
                if (f[3] == 0) {
                    if (sid == 1 && c->data[1] + L <= BIG)
                        memcpy(c->body1 + c->data[1], f + 9, L);
                    c->data[sid] += L;
                    if (f[4] & 1) c->end[sid] = 1;
                }
                if (f[3] == 1) { c->hdrs[sid] = 1; if (f[4] & 1) c->end[sid] = 1; }
                if (f[3] == 3 && L == 4) c->rst[sid] = 1 + ((f[9] << 24) | (f[10] << 16) | (f[11] << 8) | f[12]);
            }
            if (f[3] == 7 && L >= 8) c->goaway = 1 + ((f[13] << 24) | (f[14] << 16) | (f[15] << 8) | f[16]);
            off += 9 + L;
        }
        memmove(c->in, c->in + off, c->n - off); c->n -= off;
        if (c->goaway || (end_sid && c->end[end_sid])) return;
    }
}

static void expect_sz(Cl *c, const char *what, size_t got, size_t want) {
    if (got != want) {
        char m[160]; snprintf(m, sizeof m, "%s: %zu bytes (want %zu)", what, got, want);
        bad(c, m);
    }
}
static void body_ok(Cl *c) {
    expect_sz(c, "stream 1 body", c->data[1], BIG);
    if (!c->end[1]) bad(c, "stream 1 never ended");
    if (c->data[1] == BIG && memcmp(c->body1, g_big, BIG) != 0) bad(c, "stream 1 body bytes differ");
}

/* ── the cases ────────────────────────────────────────────────────────── */

/* A client on the default 65535-byte windows gets exactly 65535 bytes, then
 * the rest once it opens both windows. */
static void c_default(Cl *c) {
    hello(c, -1); get(c, 1, 1); pump(c, 800, 0);
    if (!c->hdrs[1]) bad(c, "no HEADERS");
    expect_sz(c, "DATA before any WINDOW_UPDATE", c->data[1], 65535);
    if (c->end[1]) bad(c, "stream ended inside the default window");
    wu(c, 0, BIG); wu(c, 1, BIG); pump(c, 3000, 1);
    body_ok(c);
}

/* INITIAL_WINDOW_SIZE=0: headers only; then exactly what the stream window
 * allows; then the connection window is the limit. */
static void c_zero(Cl *c) {
    hello(c, 0); get(c, 1, 1); pump(c, 800, 0);
    if (!c->hdrs[1]) bad(c, "no HEADERS");
    expect_sz(c, "DATA on a zero stream window", c->data[1], 0);
    wu(c, 1, 1000); pump(c, 800, 0);
    expect_sz(c, "DATA after a 1000-byte stream WINDOW_UPDATE", c->data[1], 1000);
    wu(c, 1, 1u << 20); pump(c, 800, 0);
    expect_sz(c, "DATA with the connection window the limit", c->data[1], 65535);
    wu(c, 0, 1u << 20); pump(c, 3000, 1);
    body_ok(c);
}

/* While stream 1 waits on its window, stream 3 is served -- with the
 * connection window it is given, and not before. */
static void c_multiplex(Cl *c) {
    hello(c, -1); get(c, 1, 1); pump(c, 800, 0);
    expect_sz(c, "stream 1 DATA", c->data[1], 65535);
    get(c, 3, 0); pump(c, 800, 0);
    if (!c->hdrs[3]) bad(c, "stream 3 not answered while stream 1 waits");
    expect_sz(c, "stream 3 DATA with the connection window spent", c->data[3], 0);
    wu(c, 0, 100); pump(c, 800, 3);
    expect_sz(c, "stream 3 DATA after the connection window opened", c->data[3], 4);
    if (!c->end[3]) bad(c, "stream 3 did not end");
    expect_sz(c, "stream 1 DATA (its own window still closed)", c->data[1], 65535);
    wu(c, 0, BIG); wu(c, 1, BIG); pump(c, 3000, 1);
    body_ok(c);
}

/* A stream reset while it waits sends nothing more; the connection goes on. */
static void c_reset(Cl *c) {
    hello(c, -1); get(c, 1, 1); pump(c, 800, 0);
    unsigned char cancel[4] = { 0, 0, 0, 8 };
    frame(c, 3, 0, 1, cancel, 4);
    wu(c, 0, BIG); wu(c, 1, BIG);
    get(c, 3, 0); pump(c, 1500, 3);
    if (!c->end[3] || c->data[3] != 4) bad(c, "stream 3 not served after the reset");
    expect_sz(c, "stream 1 DATA after RST_STREAM", c->data[1], 65535);
}

/* A peer that never opens its window does not hold the worker: the
 * connection ends within timeout_secs (2 s) of the last byte sent. */
static void c_stalled(Cl *c) {
    hello(c, -1); get(c, 1, 1); pump(c, 9000, 0);
    if (!c->eof && !c->goaway) bad(c, "connection still open after 9 s");
}

/* A WINDOW_UPDATE that takes the connection window past 2^31-1 is
 * GOAWAY(FLOW_CONTROL_ERROR) -- not a signed overflow. */
static void c_overflow(Cl *c) {
    hello(c, -1); wu(c, 0, 0x7fffffffu); pump(c, 1500, 0);
    if (c->goaway != 1 + 3) {
        char m[64]; snprintf(m, sizeof m, "GOAWAY code %d (want 3)", c->goaway - 1);
        bad(c, m);
    }
}

typedef struct { const char *name; void (*fn)(Cl *); int max_ms; } Case;
typedef struct { const Case *k; Cl *c; } Arg;

static void *client(void *arg) {
    Arg *a = arg; Cl *c = a->c;
    c->s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)g_port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c->s, (struct sockaddr *)&sa, sizeof sa) < 0) { bad(c, "connect"); return NULL; }
    c->ssl = SSL_new(g_cctx); SSL_set_fd(c->ssl, c->s);
    SSL_set_alpn_protos(c->ssl, (const unsigned char *)"\x02h2\x08http/1.1", 12);
    if (SSL_connect(c->ssl) != 1) { bad(c, "TLS"); return NULL; }
    {   /* 121.80: the server's own ALPN choice (http.c alpn_select_cb), so
         * these cases run only over a really negotiated h2 connection. */
        const unsigned char *p = NULL; unsigned int pl = 0;
        SSL_get0_alpn_selected(c->ssl, &p, &pl);
        if (pl != 2 || memcmp(p, "h2", 2) != 0) {
            bad(c, "ALPN did not negotiate h2");
            SSL_free(c->ssl); close(c->s); return NULL;
        }
    }
    a->k->fn(c);
    SSL_free(c->ssl); close(c->s);
    return NULL;
}

static int run(const Case *k) {
    static Cl c; memset(&c, 0, sizeof c);
    Arg a = { k, &c };
    pthread_t t; pthread_create(&t, NULL, client, &a);
    int fd = accept(g_lfd, NULL, NULL);
    long t0 = now_ms();
    handle_tls_connection(fd, g_sctx);
    long took = now_ms() - t0;
    pthread_join(t, NULL);
    if (k->max_ms && took > k->max_ms) {
        char m[96]; snprintf(m, sizeof m, "held the worker %ld ms (bound %d)", took, k->max_ms);
        bad(&c, m);
    }
    int ok = c.why[0] == 0;
    printf("%s %s: %s (server held the worker %ld ms)\n", ok ? "OK" : "BAD",
           k->name, ok ? "as required" : c.why, took);
    fflush(stdout);
    return ok;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) return 2;
    for (int i = 0; i < BIG; i++) g_big[i] = (char)('a' + i % 26);
    g_big[BIG] = 0;
    g_sctx = SSL_CTX_new(TLS_server_method());
    if (SSL_CTX_use_certificate_file(g_sctx, argv[1], SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(g_sctx, argv[2], SSL_FILETYPE_PEM) != 1) {
        printf("BAD tls setup\n"); return 1;
    }
    SSL_CTX_set_alpn_select_cb(g_sctx, alpn_select_cb, NULL);   /* http.c's (121.80) */
    g_cctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(g_cctx, SSL_VERIFY_NONE, NULL);

    http_set_limits(0, 4096, 2);          /* timeout_secs 2 */
    http_GET("/big", big);
    http_GET("/ping", ping);
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_lfd, 4) < 0) {
        printf("BAD listen\n"); return 1;
    }
    socklen_t al = sizeof a; getsockname(g_lfd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);

    static const Case C[] = {
        { "default-window",      c_default,   0 },
        { "initial-window-zero", c_zero,      0 },
        { "multiplexed",         c_multiplex, 0 },
        { "reset-while-waiting", c_reset,     0 },
        { "stalled-peer",        c_stalled,   4500 },
        { "window-overflow",     c_overflow,  0 },
    };
    const char *only = getenv("C600_ONLY");
    int nbad = 0;
    for (size_t i = 0; i < sizeof C / sizeof C[0]; i++) {
        if (only && !strstr(C[i].name, only)) continue;
        if (!run(&C[i])) nbad++;
    }
    printf("DONE\n");
    return nbad ? 1 : 0;
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
    # A sanitizer abort ends the harness mid-case: that case printed nothing.
    if ! grep -a -q '^DONE$' "${WORK}/out"; then
        fail "harness stopped (exit ${rc}) -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err" | head -c 300)"
    fi
fi

echo ""
echo "C600: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
