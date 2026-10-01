#!/usr/bin/env bash
# C460_http_request_framing.sh — the HTTP/1.1 server frames every request the
# way RFC 9112 says, on both the plain and the TLS connection loop, and a
# request cannot hold a worker longer than its deadline (story 121.17:
# audit-120 HTT-02, HTT-03, HTT-06).
#
# THE DEFECTS, measured on main (a28af1b) with this harness:
#
#   HTT-02  The header read pulls whatever the socket has, and a chunked body
#           that arrived in the same segment as its headers was thrown away:
#           http_chunked_read() started reading FRESH from the socket, mid-
#           body.  `POST … chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n` in one
#           write answered with the raw chunk text as the body.  A failed
#           decode kept the connection alive, so the rest of the chunk stream
#           was parsed as the next request (desync).  Bytes of a pipelined
#           second request that shared a segment with the first were
#           discarded.  The TLS loop never decoded chunked bodies at all, so
#           over TLS the chunk stream WAS the next request.
#   HTT-03  Content-Length was the first matching line only (a second,
#           different one was ignored), `Content-Length : 5` was not seen,
#           `5x` and `+5` were accepted by atoll(), and Transfer-Encoding was
#           the first header's prefix only — `gzip, chunked` was a request
#           with no body.
#   HTT-06  The only clock was SO_RCVTIMEO, which restarts on every byte, so
#           a client trickling one byte per (timeout - 1) seconds held a
#           worker for max_header x timeout.
#
# WHAT THIS DRIVES.  A C harness compiled with src/stdlib/http.c (#included,
# so the static TLS loop is reachable) under ASan+UBSan.  Each case is a
# script of byte segments written over a real loopback TCP connection to
# handle_connection() (plain) or handle_tls_connection() (TLS), and the
# answer is asserted as the exact sequence of status codes plus required
# body text.  The TLS pass needs OpenSSL headers and the openssl CLI; without
# them it is a named SKIP line and the plain pass still runs.
#
# Negative control: C460_STDLIB=<dir> points the harness at another stdlib
# tree.  A copy of src/stdlib/ with main's http.c put back fails the framing
# and deadline cases on both loops.
#
# Story: 121.17

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C460_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c460_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C460: HTTP/1.1 request framing (CL/TE, chunked, pipelining, deadline)"
echo "----------------------------------------------------------------------"

cat > "${WORK}/h.c" <<'EOF'
#include "http.c"
#include <pthread.h>
#include <sys/time.h>

static char g_echo[8192];
static Res echo(Req r) {
    const char *b = r.body ? r.body : "";
    snprintf(g_echo, sizeof g_echo, "len=%zu body=%s", strlen(b), b);
    return make_res(200, g_echo);
}
static Res ping(Req r) { (void)r; return make_res(200, "pong"); }

#define MAXSEG 20
typedef struct {
    const char *name;
    const char *seg[MAXSEG];   /* NULL-terminated */
    int         gap_ms;        /* pause before every segment after the first */
    const char *want_status;   /* e.g. "200,200" */
    const char *want_text[3];  /* substrings the response must contain */
    int         max_ms;        /* 0 = no bound on how long the server took */
    int         tls_status;    /* 0 = same as plain; 1 = TLS only checks the
                                  close time (a TLS error close may be silent) */
} Case;

static int g_port, g_lfd;
static int g_tls;
#ifdef TK_HAVE_OPENSSL
static SSL_CTX *g_sctx, *g_cctx;
#endif

typedef struct { const Case *c; char out[65536]; size_t n; } Client;

static long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static void *client(void *arg) {
    Client *cl = arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return NULL; }
    struct timeval tv = { 12, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef TK_HAVE_OPENSSL
    SSL *ssl = NULL;
    if (g_tls) {
        ssl = SSL_new(g_cctx); SSL_set_fd(ssl, s);
        if (SSL_connect(ssl) != 1) { SSL_free(ssl); close(s); return NULL; }
    }
#endif
    for (int i = 0; i < MAXSEG && cl->c->seg[i]; i++) {
        if (i > 0 && cl->c->gap_ms) usleep((useconds_t)cl->c->gap_ms * 1000);
        const char *p = cl->c->seg[i]; size_t len = strlen(p);
        ssize_t w;
#ifdef TK_HAVE_OPENSSL
        if (g_tls) w = SSL_write(ssl, p, (int)len); else
#endif
        w = send(s, p, len, 0);
        if (w <= 0) break;              /* the server has hung up */
    }
    if (!g_tls) shutdown(s, SHUT_WR);
    for (;;) {
        ssize_t r;
        size_t room = sizeof cl->out - 1 - cl->n;
        if (!room) break;
#ifdef TK_HAVE_OPENSSL
        if (g_tls) r = SSL_read(ssl, cl->out + cl->n, (int)room); else
#endif
        r = recv(s, cl->out + cl->n, room, 0);
        if (r <= 0) break;
        cl->n += (size_t)r;
    }
    cl->out[cl->n] = '\0';
#ifdef TK_HAVE_OPENSSL
    if (ssl) SSL_free(ssl);
#endif
    close(s);
    return NULL;
}

/* statuses — "200,404" from every "HTTP/1.1 NNN " in the response text. */
static void statuses(const char *o, char *dst, size_t cap) {
    dst[0] = '\0';
    for (const char *p = o; (p = strstr(p, "HTTP/1.1 ")); p += 9) {
        if (strlen(p) < 12 || p[12] != ' ') continue;
        size_t l = strlen(dst);
        snprintf(dst + l, cap - l, "%s%.3s", l ? "," : "", p + 9);
    }
}

static int run(const Case *c) {
    Client cl; memset(&cl, 0, sizeof cl); cl.c = c;
    pthread_t t; pthread_create(&t, NULL, client, &cl);
    int fd = accept(g_lfd, NULL, NULL);
    long t0 = now_ms();
#ifdef TK_HAVE_OPENSSL
    if (g_tls) handle_tls_connection(fd, g_sctx); else
#endif
    handle_connection(fd);
    long took = now_ms() - t0;
    pthread_join(t, NULL);
    char st[256]; statuses(cl.out, st, sizeof st);
    int ok = 1;
    if (!(g_tls && c->tls_status) && strcmp(st, c->want_status) != 0) ok = 0;
    for (int i = 0; i < 3 && c->want_text[i]; i++)
        if (!(g_tls && c->tls_status) && !strstr(cl.out, c->want_text[i])) ok = 0;
    if (c->max_ms && took > c->max_ms) ok = 0;
    printf("%s %s [%s]: statuses \"%s\" (want \"%s\"), %ld ms%s\n",
           ok ? "OK" : "BAD", c->name, g_tls ? "tls" : "plain", st,
           c->want_status, took, c->max_ms ? " (bound checked)" : "");
    return ok;
}

#define H "Host: t\r\n"
#define CLOSE "Connection: close\r\n"
static const Case CASES[] = {
  /* HTT-02: body in the same segment as the headers */
  { "chunked-same-segment",
    { "POST /echo HTTP/1.1\r\n" H CLOSE "Transfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n", NULL }, 0, "200", { "len=5 body=hello" }, 0, 0 },
  { "chunked-split-mid-chunk",
    { "POST /echo HTTP/1.1\r\n" H CLOSE "Transfer-Encoding: chunked\r\n\r\n5\r\nhel",
      "lo\r\n0\r\n\r\n", NULL }, 100, "200", { "len=5 body=hello" }, 0, 0 },
  { "chunked-then-pipelined-request",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\nGET /ping HTTP/1.1\r\n" H CLOSE "\r\n", NULL },
    0, "200,200", { "len=5 body=hello", "pong" }, 0, 0 },
  { "chunked-ext-and-trailer-then-pipelined",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\n\r\n"
      "5;ext=1\r\nhello\r\n0\r\nX-Trailer: 1\r\n\r\n"
      "GET /ping HTTP/1.1\r\n" H CLOSE "\r\n", NULL },
    0, "200,200", { "len=5 body=hello", "pong" }, 0, 0 },
  { "cl-then-pipelined-request",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length: 5\r\n\r\nhello"
      "GET /ping HTTP/1.1\r\n" H CLOSE "\r\n", NULL },
    0, "200,200", { "len=5 body=hello", "pong" }, 0, 0 },
  { "cl-pipelined-split-across-segments",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length: 5\r\n\r\nhelloGET /pi",
      "ng HTTP/1.1\r\n" H CLOSE "\r\n", NULL },
    100, "200,200", { "len=5 body=hello", "pong" }, 0, 0 },
  { "chunk-data-not-followed-by-crlf",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\n\r\n"
      "5\r\nhelloXX0\r\n\r\n", NULL }, 0, "400", { NULL }, 0, 0 },
  { "chunk-size-line-empty",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\n\r\n"
      "\r\nhello\r\n0\r\n\r\n", NULL }, 0, "400", { NULL }, 0, 0 },
  { "chunked-over-max-body",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\n\r\n"
      "1400\r\n", NULL }, 0, "413", { NULL }, 0, 0 },
  /* HTT-03 */
  { "cl-duplicate-differing",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length: 5\r\nContent-Length: 6\r\n\r\nhello!",
      NULL }, 0, "400", { NULL }, 0, 0 },
  { "cl-duplicate-identical",
    { "POST /echo HTTP/1.1\r\n" H CLOSE "Content-Length: 5\r\nContent-Length: 5\r\n\r\nhello",
      NULL }, 0, "200", { "len=5 body=hello" }, 0, 0 },
  { "cl-space-before-colon",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length : 5\r\n\r\nhello", NULL },
    0, "400", { NULL }, 0, 0 },
  { "cl-trailing-garbage",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length: 5x\r\n\r\nhello", NULL },
    0, "400", { NULL }, 0, 0 },
  { "cl-plus-sign",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length: +5\r\n\r\nhello", NULL },
    0, "400", { NULL }, 0, 0 },
  { "te-gzip-then-chunked",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n", NULL }, 0, "501", { NULL }, 0, 0 },
  { "te-chunked-not-final",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: chunked, gzip\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n", NULL }, 0, "400", { NULL }, 0, 0 },
  { "te-chunked-twice-across-headers",
    { "POST /echo HTTP/1.1\r\n" H "Transfer-Encoding: chunked\r\n"
      "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n", NULL },
    0, "400", { NULL }, 0, 0 },
  { "te-plus-cl",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n", NULL }, 0, "400", { NULL }, 0, 0 },
  { "obs-fold-header",
    { "POST /echo HTTP/1.1\r\n" H "X-A: 1\r\n  folded\r\nContent-Length: 5\r\n\r\nhello",
      NULL }, 0, "400", { NULL }, 0, 0 },
  /* HTT-06: timeout is 2 s; the client trickles for 6 s */
  { "slow-headers-deadline",
    { "GET /ping HTTP/1.1\r\n", "X", "-", "A", ":", " ", "1", "\r", "\n", "X",
      "-", "B", ":", " ", "2", NULL }, 400, "408", { NULL }, 3500, 1 },
  { "slow-body-deadline",
    { "POST /echo HTTP/1.1\r\n" H "Content-Length: 100\r\n\r\n", "a", "b", "c",
      "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", NULL },
    400, "408", { NULL }, 3500, 1 },
  /* controls that must stay as they are */
  { "plain-get",
    { "GET /ping HTTP/1.1\r\n" H CLOSE "\r\n", NULL }, 0, "200", { "pong" }, 0, 0 },
  { "cl-body-split",
    { "POST /echo HTTP/1.1\r\n" H CLOSE "Content-Length: 5\r\n\r\nhe", "llo", NULL },
    100, "200", { "len=5 body=hello" }, 0, 0 },
};

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    g_tls = argc > 1 && strcmp(argv[1], "tls") == 0;
#ifdef TK_HAVE_OPENSSL
    if (g_tls) {
        g_sctx = SSL_CTX_new(TLS_server_method());
        if (SSL_CTX_use_certificate_file(g_sctx, argv[2], SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(g_sctx, argv[3], SSL_FILETYPE_PEM) != 1) {
            printf("BAD tls setup\n"); return 1;
        }
        g_cctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(g_cctx, SSL_VERIFY_NONE, NULL);
    }
#else
    if (g_tls) { printf("BAD built without TK_HAVE_OPENSSL\n"); return 1; }
#endif
    http_set_limits(0, 4096, 2);
    http_POST("/echo", echo);
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
    const char *only = getenv("C460_ONLY");
    int bad = 0;
    for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        if (only && !strstr(CASES[i].name, only)) continue;
        if (!run(&CASES[i])) bad++;
    }
    return bad ? 1 : 0;
}
EOF

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.  h.c #includes http.c itself.
# 127.270: as objects, compiled once per flag set and cached -- the closure
# is built with the case's own flags (-DTK_HAVE_OPENSSL reaches it too).

# build_run NAME MODE EXTRA-CFLAGS -- EXTRA-LINK...  (reports every case line)
build_run() {
    local name="$1" mode="$2" cflags="$3"; shift 3
    local CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} ${cflags} -iquote "${S}" -I"${S}")
    local LINKSET LIBS=()
    LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c \
            --cc "${CC}" --objects http -- "${CF[@]}")" \
        || { fail "${name}: stdlib link set: see stderr above"; return; }
    while IFS= read -r l; do LIBS+=("$l"); done <<< "${LINKSET}"
    if ! "${CC}" "${CF[@]}" \
            -o "${WORK}/${name}" "${WORK}/h.c" "${LIBS[@]}" "$@" -lz -lpthread \
            >"${WORK}/${name}.cc" 2>&1; then
        fail "${name}: build: $(grep -a -m3 -E 'error' "${WORK}/${name}.cc" | head -c 400)"
        return
    fi
    "${WORK}/${name}" "${mode}" "${WORK}/cert.pem" "${WORK}/key.pem" \
        >"${WORK}/${name}.out" 2>"${WORK}/${name}.err"
    local rc=$?
    while IFS= read -r line; do
        case "${line}" in
            OK\ *)  pass "${line#OK }" ;;
            BAD\ *) fail "${line#BAD }" ;;
        esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/${name}.out")
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/${name}.out"; then
        fail "${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/${name}.err" | head -c 300)"
    fi
}

build_run plain plain ""

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
if [ -z "${OSSL_INC}" ] || ! command -v openssl >/dev/null 2>&1; then
    echo "  SKIP: TLS loop -- no OpenSSL headers or openssl CLI on this host"
else
    OSSL_LIB="$(dirname "${OSSL_INC}")/lib"
    if openssl req -x509 -newkey rsa:2048 -keyout "${WORK}/key.pem" \
           -out "${WORK}/cert.pem" -days 2 -nodes -subj "/CN=localhost" \
           >/dev/null 2>&1; then
        build_run tls tls "-DTK_HAVE_OPENSSL -Wno-deprecated-declarations -I${OSSL_INC}" \
            "-L${OSSL_LIB}" -lssl -lcrypto
    else
        fail "tls: could not generate a self-signed certificate"
    fi
fi

echo ""
echo "C460: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
