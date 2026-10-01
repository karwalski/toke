#!/usr/bin/env bash
# C491_http2_request_deadline.sh — an HTTP/2 peer cannot hold a worker by
# sending a request slowly, and cannot grow a header block without bound
# (story 121.44 item 4: the HTTP/2 counterpart of audit-120 HTT-06, which
# 121.17 fixed for HTTP/1.1 only).
#
# THE DEFECTS, measured on main (1346588c) with this harness:
#
#   The only clock on the HTTP/2 path was SO_RCVTIMEO, which restarts on
#   every byte: 5 s during the TLS handshake and preface, 30 s once the frame
#   loop starts.  A peer that trickles the connection preface, or one frame,
#   a byte at a time -- or sends a HEADERS frame without END_HEADERS and then
#   one small CONTINUATION frame every few hundred milliseconds -- held the
#   worker for as long as it kept going.  Over TLS a record trickled byte by
#   byte blocked inside SSL_read the same way.
#
#   CONTINUATION frames were appended to the stream's header block with no
#   ceiling: 100 frames of 16000 bytes grew one header block to 1.6 MB, and
#   nothing answered.
#
# NOW: the preface and the peer's first SETTINGS frame must arrive within
# timeout_secs of the connection starting; every frame, once its first byte
# has arrived, must complete within timeout_secs; a header block (HEADERS up
# to END_HEADERS) must complete within timeout_secs of its first frame; and a
# header block larger than the SETTINGS_MAX_HEADER_LIST_SIZE we advertised is
# a GOAWAY (ENHANCE_YOUR_CALM).  Waiting BETWEEN requests is not a request:
# the idle control below sleeps past timeout_secs and is still served.
#
# WHAT THIS DRIVES.  A C harness that #includes src/stdlib/http.c, run under
# ASan+UBSan.  Each case is a script of byte segments written over a real
# loopback TLS connection (ALPN h2) to handle_tls_connection(); the harness
# asserts what the client got back and how long the server held the worker.
# Needs OpenSSL headers and the openssl CLI; without them the script prints
# a named SKIP line and exits 0.
#
# Negative control: C491_STDLIB=<dir> points the harness at another stdlib
# tree.  A copy of src/stdlib/ with main's http.c/http2.c/http2.h fails the
# four deadline/cap cases and passes the two controls.
#
# 121.52 -- LINGERING CLOSE.  The server sends GOAWAY and closes while the
# client's flood is still unread or still arriving; that close answers the
# client with a TCP reset, and the reset can discard the GOAWAY before the
# client reads it.  continuation-flood saw "closed" in 6 of 20 runs of this
# script under CPU load on main (de0d95ba).  The server now half-closes after
# a GOAWAY and reads and discards until the client closes, for at most 1 s
# (http_linger_close).  flood-then-more keeps writing after the flood and
# pauses before it reads, so the reset, if there is one, always lands first;
# linger-bounded shows a client that never stops cannot hold the worker past
# the linger.  Both flood cases also assert the GOAWAY code
# (ENHANCE_YOUR_CALM).  Against main's http.c the flood cases fail in most
# runs, not every run -- a reset loses the GOAWAY only when it overtakes it.
#
# Story: 121.44, 121.52

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C491_STDLIB:-${REPO_ROOT}/src/stdlib}"
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c http)" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c491_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C491: HTTP/2 request deadline and header-block ceiling"
echo "------------------------------------------------------"

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
if [ -z "${OSSL_INC}" ] || ! command -v openssl >/dev/null 2>&1; then
    echo "  SKIP: HTTP/2 needs TLS here -- no OpenSSL headers or openssl CLI on this host"
    echo ""
    echo "C491: 0 passed, 0 failed (skipped)"
    exit 0
fi
OSSL_LIB="$(dirname "${OSSL_INC}")/lib"

cat > "${WORK}/h.c" <<'EOF'
#include "http.c"
#include <pthread.h>
#include <sys/time.h>

static Res ping(Req r) { (void)r; return make_res(200, "pong"); }

#define MAXSEG 128
typedef struct {
    const char    *name;
    unsigned char *seg[MAXSEG];
    size_t         len[MAXSEG];
    int            nseg;
    int            gap_ms;       /* pause before every segment after the first */
    const char    *want;         /* "served", "goaway", "closed" or "any"      */
    int            max_ms;       /* 0 = no bound on how long the server took   */
    int            code;         /* GOAWAY error code wanted (with "goaway")   */
    int            read_delay_ms;/* pause after the last write, before reading */
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
static const unsigned char GETPING[] = { 0x82, 0x87, 0x44, 5, '/','p','i','n','g', 0x41, 1, 't' };
/* preface + empty SETTINGS */
static size_t hello(unsigned char *o) {
    memcpy(o, PREFACE, 24);
    return 24 + frame(o + 24, 4, 0, 0, NULL, 0);
}
static size_t get_ping(unsigned char *o, unsigned sid) {
    return frame(o, 1, 0x5 /* END_STREAM|END_HEADERS */, sid, GETPING, sizeof GETPING);
}

/* ── client ───────────────────────────────────────────────────────────── */
typedef struct { const Case *c; unsigned char out[1 << 16]; size_t n; } Client;

static void *client(void *arg) {
    Client *cl = arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return NULL; }
    struct timeval tv = { 14, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    SSL *ssl = SSL_new(g_cctx); SSL_set_fd(ssl, s);
    SSL_set_alpn_protos(ssl, (const unsigned char *)"\x02h2", 3);
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); close(s); return NULL; }
    for (int i = 0; i < cl->c->nseg; i++) {
        if (i > 0 && cl->c->gap_ms) usleep((useconds_t)cl->c->gap_ms * 1000);
        if (SSL_write(ssl, cl->c->seg[i], (int)cl->c->len[i]) <= 0) break;
    }
    /* 121.52: a pause here lets any TCP reset from the server arrive before
     * the first read, so a GOAWAY a reset would discard is lost every time
     * rather than when the scheduler happens to allow it. */
    if (cl->c->read_delay_ms) usleep((useconds_t)cl->c->read_delay_ms * 1000);
    /* Read frames until the response ends, a GOAWAY, EOF or 14 s. */
    for (;;) {
        size_t room = sizeof cl->out - cl->n;
        if (!room) break;
        int r = SSL_read(ssl, cl->out + cl->n, (int)room);
        if (r <= 0) break;
        cl->n += (size_t)r;
        int done = 0;
        for (size_t p = 0; p + 9 <= cl->n; ) {
            size_t L = ((size_t)cl->out[p] << 16) | ((size_t)cl->out[p+1] << 8) | cl->out[p+2];
            if (p + 9 + L > cl->n) break;
            if (cl->out[p+3] == 7) done = 1;                              /* GOAWAY */
            if (cl->out[p+3] == 0 && (cl->out[p+4] & 1)) done = 1;        /* DATA+ES */
            p += 9 + L;
        }
        if (done) break;
    }
    SSL_free(ssl);
    close(s);
    return NULL;
}

static int saw(const Client *cl, int type) {
    for (size_t p = 0; p + 9 <= cl->n; ) {
        size_t L = ((size_t)cl->out[p] << 16) | ((size_t)cl->out[p+1] << 8) | cl->out[p+2];
        if (p + 9 + L > cl->n) break;
        if (cl->out[p+3] == type) return 1;
        p += 9 + L;
    }
    return 0;
}
/* the error code of the first GOAWAY frame, or -1 */
static long goaway_code(const Client *cl) {
    for (size_t p = 0; p + 9 <= cl->n; ) {
        size_t L = ((size_t)cl->out[p] << 16) | ((size_t)cl->out[p+1] << 8) | cl->out[p+2];
        if (p + 9 + L > cl->n) break;
        if (cl->out[p+3] == 7 && L >= 8) {
            const unsigned char *e = cl->out + p + 9 + 4;
            return ((long)e[0] << 24) | ((long)e[1] << 16) | ((long)e[2] << 8) | e[3];
        }
        p += 9 + L;
    }
    return -1;
}
static int has_pong(const Client *cl) {
    for (size_t i = 0; i + 4 <= cl->n; i++)
        if (memcmp(cl->out + i, "pong", 4) == 0) return 1;
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
    const char *got = has_pong(&cl) ? "served" : saw(&cl, 7) ? "goaway" : "closed";
    long code = goaway_code(&cl);
    int ok = (strcmp(c->want, "any") == 0 || strcmp(got, c->want) == 0) &&
             (!c->max_ms || took <= c->max_ms) &&
             (strcmp(c->want, "goaway") != 0 || code == c->code);
    printf("%s %s: %s (want %s), GOAWAY code %ld, server held the worker %ld ms%s\n",
           ok ? "OK" : "BAD", c->name, got, c->want, code, took,
           c->max_ms ? " (bound checked)" : "");
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

    http_set_limits(0, 4096, 2);          /* timeout_secs = 2 */
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

    static unsigned char b[1 << 15];
    size_t n;
    static Case C[8];
    int k = 0;

    /* control: a whole request at once is served */
    C[k].name = "control-get"; C[k].want = "served"; C[k].max_ms = 0;
    n = hello(b); n += get_ping(b + n, 1); add(&C[k], b, n); k++;

    /* control: idle BETWEEN requests is not a request -- sleep 3 s (> the
     * 2 s timeout) after SETTINGS, then ask; still served */
    C[k].name = "control-idle-then-get"; C[k].want = "served"; C[k].gap_ms = 3000;
    n = hello(b); add(&C[k], b, n); n = get_ping(b, 1); add(&C[k], b, n); k++;

    /* the preface, one byte every 400 ms (24 bytes: 9.6 s) */
    C[k].name = "preface-trickle"; C[k].want = "closed"; C[k].gap_ms = 400;
    C[k].max_ms = 3500;
    for (int i = 0; i < 24; i++) add(&C[k], PREFACE + i, 1);
    k++;

    /* preface and SETTINGS at once, then one HEADERS frame a byte at a time */
    C[k].name = "frame-trickle"; C[k].want = "closed"; C[k].gap_ms = 400;
    C[k].max_ms = 3500;
    n = hello(b); add(&C[k], b, n);
    n = get_ping(b, 1);
    for (size_t i = 0; i < n && C[k].nseg < MAXSEG; i++) add(&C[k], b + i, 1);
    k++;

    /* HEADERS without END_HEADERS, then a 1-byte CONTINUATION every 400 ms:
     * every frame is whole and on time, the header block never ends */
    C[k].name = "continuation-trickle"; C[k].want = "closed"; C[k].gap_ms = 400;
    C[k].max_ms = 3500;
    n = hello(b); n += frame(b + n, 1, 0x1, 1, GETPING, 1); add(&C[k], b, n);
    for (int i = 0; i < 24; i++) {
        unsigned char x = 0x86; n = frame(b, 9, 0, 1, &x, 1); add(&C[k], b, n);
    }
    k++;

    /* HEADERS without END_HEADERS, then 100 x 16000-byte CONTINUATION frames
     * (1.6 MB) at once: past the 65536 we advertise -> GOAWAY */
    C[k].name = "continuation-flood"; C[k].want = "goaway"; C[k].max_ms = 0;
    C[k].code = 0xb;   /* ENHANCE_YOUR_CALM */
    C[k].read_delay_ms = 200;
    n = hello(b); n += frame(b + n, 1, 0x1, 1, GETPING, 1); add(&C[k], b, n);
    {
        static unsigned char pay[16000]; memset(pay, 0x86, sizeof pay);
        for (int i = 0; i < 100; i++) { n = frame(b, 9, 0, 1, pay, sizeof pay); add(&C[k], b, n); }
    }
    k++;

    /* 121.52: the same flood, and then the client keeps writing -- three
     * more CONTINUATION frames 100 ms apart -- before it reads.  The server
     * has sent GOAWAY and wants to close; closing with the client's bytes
     * still arriving answers them with a TCP reset, and the reset can
     * discard the GOAWAY before the client reads it.  The server must close
     * lingering: half-close, then read and discard until the client is done. */
    C[k].name = "flood-then-more"; C[k].want = "goaway"; C[k].gap_ms = 100;
    C[k].code = 0xb; C[k].max_ms = 0; C[k].read_delay_ms = 200;
    {
        static unsigned char pay[16000]; memset(pay, 0x86, sizeof pay);
        size_t big = 64 + 101 * (9 + sizeof pay);
        unsigned char *f = malloc(big);
        n = hello(f); n += frame(f + n, 1, 0x1, 1, GETPING, 1);
        for (int i = 0; i < 100; i++) n += frame(f + n, 9, 0, 1, pay, sizeof pay);
        add(&C[k], f, n); free(f);
        for (int i = 0; i < 3; i++) { n = frame(b, 9, 0, 1, pay, sizeof pay); add(&C[k], b, n); }
    }
    k++;

    /* 121.52: the linger is bounded.  A client that floods (80 KB at once)
     * and then never stops writing (a frame every 300 ms for 6 s) and never closes cannot
     * hold the worker: the server gives up lingering and closes.  What the
     * client sees after that is not the point -- only the bound is. */
    C[k].name = "linger-bounded"; C[k].want = "any"; C[k].gap_ms = 300;
    C[k].max_ms = 2500;   /* the 1 s linger, plus margin for a loaded host */
    {
        static unsigned char pay[16000]; memset(pay, 0x86, sizeof pay);
        unsigned char *f = malloc(64 + 5 * (9 + sizeof pay));
        n = hello(f); n += frame(f + n, 1, 0x1, 1, GETPING, 1);
        for (int i = 0; i < 5; i++) n += frame(f + n, 9, 0, 1, pay, sizeof pay);
        add(&C[k], f, n); free(f);
        for (int i = 0; i < 20; i++) { n = frame(b, 9, 0, 1, pay, 1); add(&C[k], b, n); }
    }
    k++;

    const char *only = getenv("C491_ONLY");
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
elif ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DTK_HAVE_OPENSSL \
        -Wno-deprecated-declarations -I"${OSSL_INC}" -iquote "${S}" -I"${S}" \
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
echo "C491: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
