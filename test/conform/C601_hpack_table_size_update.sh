#!/usr/bin/env bash
# C601_hpack_table_size_update.sh -- when the peer changes
# SETTINGS_HEADER_TABLE_SIZE, the server's HPACK encoder signals the change
# with a dynamic table size update at the start of its next header block
# (RFC 7541 4.2, 6.3), and never grows past the size the peer's decoder uses
# (story 121.50 item 2).
#
# THE DEFECT, measured on main (f605aec9) with this harness: h2_apply_settings
# resized the encoder's table to whatever the peer advertised and said
# nothing.  So (a) a peer that lowered the size received blocks with no size
# update -- strict decoders (nghttp2) reject them; (b) a peer that raised it
# had the encoder's table grow past 4096 bytes while its decoder, which
# follows only size updates, stayed at 4096 -- once more than 4096 bytes of
# fields were indexed the two tables disagreed and responses decoded to the
# wrong header values; (c) a size lowered and raised again between two blocks
# emptied the encoder's table silently, while the decoder kept its entries.
#
# NOW: hpack_encoder_set_limit() sets the encoder's table to
# min(advertised, 4096) and records the change; the next block opens with
# the smallest size passed through (when it is below the final one) and then
# the final size.
#
# WHAT THIS DRIVES.  A C harness that #includes src/stdlib/http.c (as C500
# does), under ASan+UBSan, over a real loopback TLS connection (ALPN h2).
# The client decodes every response header block with the stdlib's own
# hpack_decode() into a table of its own, and is STRICT the way nghttp2 is:
# when it has advertised less than its table's current size, a block that does
# not open with a size update is rejected.  Needs OpenSSL headers and the
# openssl CLI; without them the script prints a named SKIP line.
#
# 121.80: the server context uses http.c's own ALPN callback (alpn_select_cb)
# and the client offers h2 and http/1.1, as a browser does; a case runs only
# when h2 was really negotiated.  Against a stdlib whose ALPN offers only
# http/1.1 (main before 121.80) every case fails "ALPN did not negotiate h2".
#
# Negative control: C601_STDLIB=<dir> points the harness at another stdlib
# tree.  A copy of src/stdlib/ with main's http.c/http2.c/http2.h fails every
# case except control-default.
#
# Story: 121.50

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C601_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c601_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C601: HPACK dynamic table size updates follow SETTINGS_HEADER_TABLE_SIZE"
echo "------------------------------------------------------------------------"

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
if [ -z "${OSSL_INC}" ] || ! command -v openssl >/dev/null 2>&1; then
    echo "  SKIP: HTTP/2 needs TLS here -- no OpenSSL headers or openssl CLI on this host"
    echo ""
    echo "C601: 0 passed, 0 failed (skipped)"
    exit 0
fi
OSSL_LIB="$(dirname "${OSSL_INC}")/lib"

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DTK_HAVE_OPENSSL -DH2_IDLE_TIMEOUT_S=3 -Wno-deprecated-declarations -Wno-macro-redefined -I"${OSSL_INC}" -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c --cc "${CC}" --objects http -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

cat > "${WORK}/h.c" <<'EOF'
#include "http.c"
#include <pthread.h>
#include <sys/time.h>
#include <poll.h>

/* GET /h?n=<k> answers with header x-v: a 200-byte value derived from k, so
 * every request indexes a new ~240-byte entry in the encoder's table. */
static char g_val[256];
static StrPair g_hdr[1];
static void value_for(int k, char *out) {
    snprintf(out, 256, "v%04d-", k);
    for (int i = 6; i < 200; i++) out[i] = (char)('a' + (k * 7 + i) % 26);
    out[200] = 0;
}
static Res hv(Req r) {
    const char *q = r.path ? strstr(r.path, "n=") : NULL;
    value_for(q ? atoi(q + 2) : 0, g_val);
    Res res = make_res(200, "ok");
    g_hdr[0].key = "x-v"; g_hdr[0].val = g_val;
    res.headers.data = g_hdr; res.headers.len = 1;
    return res;
}

static int g_port, g_lfd;
static SSL_CTX *g_sctx, *g_cctx;
static long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

typedef struct {
    SSL *ssl; int s;
    unsigned char in[1 << 16]; size_t n;
    unsigned char blk[1 << 14]; size_t blk_len; int got_block;
    HpackTable dec;       /* the client's decoder */
    size_t adv;           /* the size the client advertised last */
    unsigned sid;
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
/* SETTINGS carrying HEADER_TABLE_SIZE values in order (none when n == 0) */
static void settings(Cl *c, const unsigned *v, int n) {
    unsigned char p[36];
    for (int i = 0; i < n; i++) {
        unsigned char *q = p + 6 * i;
        q[0] = 0; q[1] = 1; q[2] = (unsigned char)(v[i] >> 24);
        q[3] = (unsigned char)(v[i] >> 16); q[4] = (unsigned char)(v[i] >> 8);
        q[5] = (unsigned char)v[i];
        c->adv = v[i];
    }
    frame(c, 4, 0, 0, p, (size_t)(6 * n));
}
static void hello(Cl *c) {
    SSL_write(c->ssl, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24);
}

/* read frames until the SETTINGS ACK (ack=1) or stream sid's HEADERS */
static void pump(Cl *c, int want_ack, unsigned sid) {
    long t0 = now_ms();
    c->got_block = 0;
    for (;;) {
        if (now_ms() - t0 > 5000) { bad(c, "timed out waiting for the server"); return; }
        if (!SSL_pending(c->ssl)) {
            struct pollfd p = { c->s, POLLIN, 0 };
            if (poll(&p, 1, 50) == 0) continue;
        }
        int r = SSL_read(c->ssl, c->in + c->n, (int)(sizeof c->in - c->n));
        if (r <= 0) { bad(c, "connection closed"); return; }
        c->n += (size_t)r;
        size_t off = 0; int done = 0;
        while (off + 9 <= c->n) {
            unsigned char *f = c->in + off;
            size_t L = ((size_t)f[0] << 16) | ((size_t)f[1] << 8) | f[2];
            if (off + 9 + L > c->n) break;
            unsigned fs = ((unsigned)(f[5] & 0x7f) << 24) | ((unsigned)f[6] << 16)
                        | ((unsigned)f[7] << 8) | f[8];
            if (f[3] == 4 && (f[4] & 1) && want_ack) done = 1;
            if (f[3] == 1 && fs == sid && L <= sizeof c->blk) {
                memcpy(c->blk, f + 9, L); c->blk_len = L; c->got_block = 1; done = 1;
            }
            if (f[3] == 7) { bad(c, "GOAWAY"); done = 1; }
            off += 9 + L;
        }
        memmove(c->in, c->in + off, c->n - off); c->n -= off;
        if (done) return;
    }
}

/* request k on the next stream; decode its response block strictly and
 * check x-v.  Returns 0 on a failure (recorded). */
static int request(Cl *c, int k) {
    char path[32]; int pl = snprintf(path, sizeof path, "/h?n=%d", k);
    unsigned char h[64]; size_t n = 0;
    h[n++] = 0x82; h[n++] = 0x87; h[n++] = 0x04; h[n++] = (unsigned char)pl;
    memcpy(h + n, path, (size_t)pl); n += (size_t)pl;
    h[n++] = 0x01; h[n++] = 1; h[n++] = 't';
    c->sid = c->sid ? c->sid + 2 : 1;
    frame(c, 1, 0x5, c->sid, h, n);
    pump(c, 0, c->sid);
    if (!c->got_block) { bad(c, "no response header block"); return 0; }
    char m[160];
    /* nghttp2's rule: advertised below the table's size -> the block must
     * open with a dynamic table size update. */
    if (c->adv < c->dec.max_size && (c->blk[0] & 0xE0) != 0x20) {
        snprintf(m, sizeof m, "request %d: block opens 0x%02x, no size update (advertised %zu, table %zu)",
                 k, c->blk[0], c->adv, c->dec.max_size);
        bad(c, m); return 0;
    }
    char **nm = NULL, **vl = NULL;
    c->dec.limit = c->adv;     /* an update may not exceed what we advertised */
    int cnt = hpack_decode(&c->dec, c->blk, c->blk_len, &nm, &vl);
    if (cnt < 0) { snprintf(m, sizeof m, "request %d: block does not decode", k); bad(c, m); return 0; }
    char want[256]; value_for(k, want);
    int ok = 0;
    for (int i = 0; i < cnt; i++) {
        if (strcmp(nm[i], "x-v") == 0 && strcmp(vl[i], want) == 0) ok = 1;
        free(nm[i]); free(vl[i]);
    }
    free(nm); free(vl);
    if (!ok) { snprintf(m, sizeof m, "request %d: x-v decoded to the wrong value", k); bad(c, m); }
    return ok;
}
static void expect_first(Cl *c, const unsigned char *want, size_t n, const char *what) {
    if (c->blk_len < n || memcmp(c->blk, want, n) != 0) {
        char m[160]; snprintf(m, sizeof m, "%s: block opens %02x %02x", what,
                              c->blk_len > 0 ? c->blk[0] : 0, c->blk_len > 1 ? c->blk[1] : 0);
        bad(c, m);
    }
}

/* ── cases ────────────────────────────────────────────────────────────── */
static void c_control(Cl *c) {          /* default size: nothing to signal */
    hello(c); settings(c, NULL, 0);
    for (int k = 0; k < 30 && request(c, k); k++) ;
}
static void c_zero(Cl *c) {             /* lowered to 0 before any request */
    unsigned v[1] = { 0 };
    hello(c); settings(c, v, 1); pump(c, 1, 0);
    if (request(c, 1)) { static const unsigned char w[] = { 0x20 }; expect_first(c, w, 1, "size 0"); }
    for (int k = 2; k < 6 && request(c, k); k++) ;
}
static void c_mid(Cl *c) {              /* lowered to 300 after the table filled */
    unsigned v[1] = { 300 };
    hello(c); settings(c, NULL, 0);
    for (int k = 0; k < 10 && request(c, k); k++) ;
    settings(c, v, 1); pump(c, 1, 0);
    for (int k = 10; k < 20 && request(c, k); k++) ;
}
static void c_raised(Cl *c) {           /* raised to 65536: stays at 4096 */
    unsigned v[1] = { 65536 };
    hello(c); settings(c, v, 1); pump(c, 1, 0);
    for (int k = 0; k < 40 && request(c, k); k++) ;
}
static void c_down_up(Cl *c) {          /* 0 then 4096 between two blocks */
    unsigned v[2] = { 0, 4096 };
    hello(c); settings(c, NULL, 0);
    for (int k = 0; k < 5 && request(c, k); k++) ;
    settings(c, v, 2); pump(c, 1, 0);
    /* size update 0, then size update 4096 (0x3f 0xe1 0x1f) */
    if (request(c, 0)) {
        static const unsigned char w[] = { 0x20, 0x3f, 0xe1, 0x1f };
        expect_first(c, w, sizeof w, "0 then 4096");
    }
    for (int k = 1; k < 5 && request(c, k); k++) ;
}

typedef struct { const char *name; void (*fn)(Cl *); } Case;
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
    hpack_table_init(&c->dec, 4096);
    c->adv = 4096;
    a->k->fn(c);
    hpack_table_free(&c->dec);
    SSL_free(c->ssl); close(c->s);
    return NULL;
}

static int run(const Case *k) {
    static Cl c; memset(&c, 0, sizeof c);
    Arg a = { k, &c };
    pthread_t t; pthread_create(&t, NULL, client, &a);
    int fd = accept(g_lfd, NULL, NULL);
    handle_tls_connection(fd, g_sctx);
    pthread_join(t, NULL);
    int ok = c.why[0] == 0;
    printf("%s %s: %s\n", ok ? "OK" : "BAD", k->name, ok ? "as required" : c.why);
    fflush(stdout);
    return ok;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc < 3) return 2;
    g_sctx = SSL_CTX_new(TLS_server_method());
    if (SSL_CTX_use_certificate_file(g_sctx, argv[1], SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(g_sctx, argv[2], SSL_FILETYPE_PEM) != 1) {
        printf("BAD tls setup\n"); return 1;
    }
    SSL_CTX_set_alpn_select_cb(g_sctx, alpn_select_cb, NULL);   /* http.c's (121.80) */
    g_cctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(g_cctx, SSL_VERIFY_NONE, NULL);
    http_set_limits(0, 4096, 2);
    http_GET("/h", hv);
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
        { "control-default",           c_control },
        { "lowered-to-zero",           c_zero },
        { "lowered-mid-connection",    c_mid },
        { "raised-past-default",       c_raised },
        { "lowered-then-raised",       c_down_up },
    };
    const char *only = getenv("C601_ONLY");
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
elif ! "${CC}" "${CF[@]}" -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" "-L${OSSL_LIB}" -lssl -lcrypto -lz -lpthread \
        >"${WORK}/cc.log" 2>&1; then
    fail "build: $(grep -a -m3 error "${WORK}/cc.log" | head -c 400)"
else
    "${WORK}/h" "${WORK}/cert.pem" "${WORK}/key.pem" >"${WORK}/out" 2>"${WORK}/err"; rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
    if ! grep -a -q '^DONE$' "${WORK}/out"; then
        fail "harness stopped (exit ${rc}) -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err" | head -c 300)"
    fi
fi

echo ""
echo "C601: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
