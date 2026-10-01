#!/usr/bin/env bash
# C602_http1_handler_security_headers.sh -- over HTTP/1.1 (plain and TLS), a
# security header the handler sets itself is sent once, with the handler's
# value; the default is sent only when the handler did not set that field
# (story 121.50 item 3).
#
# THE DEFECT, measured on main (f605aec9) with this harness: every HTTP/1.1
# response path wrote the handler's headers and then the fixed security block
# (X-Content-Type-Options, X-Frame-Options, Referrer-Policy, and HSTS over
# TLS) unconditionally.  A handler that set X-Frame-Options: SAMEORIGIN sent
# SAMEORIGIN and DENY -- and a browser that sees two X-Frame-Options values
# applies neither.  HTTP/2 already let the handler win (121.46).
#
# NOW: write_security_headers / write_security_headers_ssl skip a field the
# handler set (names compared without regard to case).
#
# WHAT THIS DRIVES.  A C harness that #includes src/stdlib/http.c, under
# ASan+UBSan: http_handle_fd() over a socketpair for plain HTTP, and
# handle_tls_connection() over loopback TLS (ALPN http/1.1) -- the body,
# HEAD and gzip paths of each.  Without OpenSSL headers the TLS half is a
# named SKIP.
#
# Negative control: C602_STDLIB=<dir> points the harness at another stdlib
# tree.  A copy of src/stdlib/ with main's http.c fails every "handler" case
# (each field counted twice); every "default" case passes on both.
#
# Story: 121.50

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C602_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c602_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C602: HTTP/1.1 security headers -- the handler's own value wins"
echo "----------------------------------------------------------------"

OSSL_INC=""
for d in /opt/homebrew/include /usr/local/include /usr/include; do
    [ -f "${d}/openssl/ssl.h" ] && { OSSL_INC="${d}"; break; }
done
TLSFLAGS=""; TLSLIBS=""
if [ -n "${OSSL_INC}" ] && command -v openssl >/dev/null 2>&1; then
    TLSFLAGS="-DTK_HAVE_OPENSSL -DC602_TLS -I${OSSL_INC}"
    TLSLIBS="-L$(dirname "${OSSL_INC}")/lib -lssl -lcrypto"
else
    echo "  SKIP: the TLS half -- no OpenSSL headers or openssl CLI on this host"
fi

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} ${TLSFLAGS} -Wno-deprecated-declarations -Wno-macro-redefined -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c --cc "${CC}" --objects http -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

cat > "${WORK}/h.c" <<'EOF'
#include "http.c"
#include <pthread.h>
#include <poll.h>

static char g_body[1024];
static StrPair g_h[3];
/* handler: sets X-Frame-Options (lower-case name), HSTS and Referrer-Policy */
static Res own(Req r) {
    (void)r;
    Res res = make_res(200, g_body);
    g_h[0].key = "x-frame-options";           g_h[0].val = "SAMEORIGIN";
    g_h[1].key = "Strict-Transport-Security"; g_h[1].val = "max-age=60";
    g_h[2].key = "REFERRER-POLICY";           g_h[2].val = "no-referrer";
    res.headers.data = g_h; res.headers.len = 3;
    return res;
}
static Res plain(Req r) { (void)r; return make_res(200, g_body); }

/* count lines of the response head whose field name is `name` (any case);
 * the first such line's value goes to val. */
static int count(const char *resp, const char *name, char *val, size_t vcap) {
    const char *end = strstr(resp, "\r\n\r\n");
    size_t n = strlen(name); int c = 0;
    const char *p = strstr(resp, "\r\n");
    while (p && p < end) {
        p += 2;
        if (strncasecmp(p, name, n) == 0 && p[n] == ':') {
            if (!c && val) {
                const char *v = p + n + 1; while (*v == ' ') v++;
                size_t k = (size_t)(strstr(v, "\r\n") - v);
                if (k >= vcap) k = vcap - 1;
                memcpy(val, v, k); val[k] = 0;
            }
            c++;
        }
        p = strstr(p, "\r\n");
    }
    return c;
}

static int nbad;
/* check the head of resp: each field once, with the expected value */
static void check(const char *name, const char *resp, int tls, int own_hdrs) {
    struct { const char *f, *dflt, *mine; int tls_only; } F[] = {
        { "X-Content-Type-Options", "nosniff", "nosniff", 0 },
        { "X-Frame-Options", "DENY", "SAMEORIGIN", 0 },
        { "Referrer-Policy", "strict-origin-when-cross-origin", "no-referrer", 0 },
        { "Strict-Transport-Security", "max-age=63072000; includeSubDomains; preload",
          "max-age=60", 1 },
    };
    char why[512] = "";
    if (!strstr(resp, "\r\n\r\n")) snprintf(why, sizeof why, " no response head");
    else for (size_t i = 0; i < 4; i++) {
        char v[128] = "";
        int c = count(resp, F[i].f, v, sizeof v);
        const char *want = own_hdrs ? F[i].mine : F[i].dflt;
        int want_n = 1;
        if (F[i].tls_only && !tls) {
            /* plain HTTP sends no HSTS of its own; a handler's passes through */
            want_n = own_hdrs ? 1 : 0;
        }
        if (c != want_n || (want_n && strcmp(v, want) != 0))
            snprintf(why + strlen(why), sizeof why - strlen(why),
                     " %s x%d '%s' (want x%d '%s')", F[i].f, c, v, want_n, want_n ? want : "");
    }
    printf("%s %s:%s\n", why[0] ? "BAD" : "OK", name, why[0] ? why : " as required");
    if (why[0]) nbad++;
}

/* ── plain: http_handle_fd over a socketpair ─────────────────────────── */
static char g_resp[1 << 16];
static void plain_case(const char *name, const char *req, int own_hdrs) {
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    write(sv[0], req, strlen(req));
    shutdown(sv[0], SHUT_WR);
    http_handle_fd(sv[1]);
    size_t n = 0; ssize_t r;
    while (n < sizeof g_resp - 1 && (r = read(sv[0], g_resp + n, sizeof g_resp - 1 - n)) > 0) n += (size_t)r;
    g_resp[n] = 0;
    close(sv[0]);
    check(name, g_resp, 0, own_hdrs);
}

#ifdef C602_TLS
static SSL_CTX *g_sctx, *g_cctx; static int g_port, g_lfd; static const char *g_req;
static void *tls_client(void *arg) {
    (void)arg;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_resp[0] = 0;
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return NULL; }
    SSL *ssl = SSL_new(g_cctx); SSL_set_fd(ssl, s);
    if (SSL_connect(ssl) != 1) { SSL_free(ssl); close(s); return NULL; }
    SSL_write(ssl, g_req, (int)strlen(g_req));
    size_t n = 0; int r;
    while (n < sizeof g_resp - 1 && (r = SSL_read(ssl, g_resp + n, (int)(sizeof g_resp - 1 - n))) > 0) n += (size_t)r;
    g_resp[n] = 0;
    SSL_free(ssl); close(s);
    return NULL;
}
static void tls_case(const char *name, const char *req, int own_hdrs) {
    g_req = req;
    pthread_t t; pthread_create(&t, NULL, tls_client, NULL);
    int fd = accept(g_lfd, NULL, NULL);
    handle_tls_connection(fd, g_sctx);
    pthread_join(t, NULL);
    check(name, g_resp, 1, own_hdrs);
}
#endif

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    (void)argc; (void)argv;
    memset(g_body, 'b', 600);            /* over GZIP_MIN_SIZE */
    http_GET("/own", own);
    http_GET("/plain", plain);
    const char *G  = "GET /own HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    const char *H  = "HEAD /own HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    const char *Z  = "GET /own HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n";
    const char *DG = "GET /plain HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
    const char *DZ = "GET /plain HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n";
    plain_case("plain handler body", G, 1);
    plain_case("plain handler HEAD", H, 1);
    plain_case("plain handler gzip", Z, 1);
    plain_case("plain default body", DG, 0);
    plain_case("plain default gzip", DZ, 0);
#ifdef C602_TLS
    if (argc < 3) return 2;
    g_sctx = SSL_CTX_new(TLS_server_method());
    if (SSL_CTX_use_certificate_file(g_sctx, argv[1], SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(g_sctx, argv[2], SSL_FILETYPE_PEM) != 1) {
        printf("BAD tls setup\n"); return 1;
    }
    g_cctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(g_cctx, SSL_VERIFY_NONE, NULL);
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_lfd, (struct sockaddr *)&a, sizeof a) < 0 || listen(g_lfd, 4) < 0) {
        printf("BAD listen\n"); return 1;
    }
    socklen_t al = sizeof a; getsockname(g_lfd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
    tls_case("tls handler body", G, 1);
    tls_case("tls handler HEAD", H, 1);
    tls_case("tls handler gzip", Z, 1);
    tls_case("tls default body", DG, 0);
    tls_case("tls default gzip", DZ, 0);
#endif
    printf("DONE\n");
    return nbad ? 1 : 0;
}
EOF

if [ -n "${TLSFLAGS}" ] && ! openssl req -x509 -newkey rsa:2048 -keyout "${WORK}/key.pem" \
        -out "${WORK}/cert.pem" -days 2 -nodes -subj "/CN=localhost" >/dev/null 2>&1; then
    fail "could not generate a self-signed certificate"
elif ! "${CC}" "${CF[@]}" -o "${WORK}/h" "${WORK}/h.c" "${LINK[@]}" ${TLSLIBS} -lz -lpthread \
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
echo "C602: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
