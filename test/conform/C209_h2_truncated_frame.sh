#!/usr/bin/env bash
# C209_h2_truncated_frame.sh — the HTTP/2 server survives a peer that sends a
# frame header and closes before the payload (story 135.3j (6)).
#
# THE DEFECT (measured on main before 135.3j (6)).  The -DTK_HAVE_OPENSSL
# branch of http.c was analysed by nothing; clang's analyzer, once pointed at
# it, reports null dereferences in handle_h2_connection.  They are real:
# h2_frame_recv returns NULL with the frame header filled in when the payload
# read fails (and when the frame is over the size limit, or the buffer cannot
# grow), and the frame loop only treated NULL as "no frame" when the header was
# all zero.  Anything else went to the frame handlers with payload == NULL:
# HEADERS/CONTINUATION memcpy'd from it, a padded HEADERS and WINDOW_UPDATE
# read payload[0].  A TLS client that completes the handshake, sends the
# connection preface and a 9-byte HEADERS header announcing 10 bytes, and
# closes, crashes the worker.
#
# WHAT THIS DRIVES (ASan+UBSan build of http.c with -DTK_HAVE_OPENSSL,
# #included for the static handle_h2_connection): a server process accepts
# TLS on one end of a socketpair and runs handle_h2_connection; a client
# process does exactly the above.  Expected: handle_h2_connection returns and
# the server exits 0; the same for a WINDOW_UPDATE header announcing 4 bytes.
# Control: a complete PING (8 bytes), then close, returns cleanly too.
#
# Negative control: C209_STDLIB=<dir> builds against another stdlib tree;
# main's http.c/http2.c die with an ASan SEGV in the HEADERS handler.
#
# Story: 135.3j

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C209_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c209_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C209: a truncated HTTP/2 frame ends the connection, not the worker"
echo "------------------------------------------------------------------"

OSSL_CFLAGS=""; OSSL_LIBS=""
if pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in "$(brew --prefix openssl@3 2>/dev/null)" /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
        if [ -n "${pfx}" ] && [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
if [ -z "${OSSL_LIBS}" ]; then
    echo "SKIP: no OpenSSL headers on this host"
    echo "CONFORM-SKIP: OpenSSL not installed"
    exit 0
fi

cat > "${WORK}/t.c" <<C
#include "${S}/http.c"
#include <sys/wait.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

/* A throwaway self-signed P-256 certificate, made in memory. */
static int make_cert(SSL_CTX *ctx) {
    EVP_PKEY *k = EVP_EC_gen("P-256");
    X509 *x = X509_new();
    if (!k || !x) return -1;
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 3600);
    X509_set_pubkey(x, k);
    X509_NAME_add_entry_by_txt(X509_get_subject_name(x), "CN", MBSTRING_ASC,
                               (const unsigned char *)"localhost", -1, -1, 0);
    X509_set_issuer_name(x, X509_get_subject_name(x));
    if (!X509_sign(x, k, EVP_sha256())) return -1;
    if (SSL_CTX_use_certificate(ctx, x) != 1 || SSL_CTX_use_PrivateKey(ctx, k) != 1) return -1;
    X509_free(x); EVP_PKEY_free(k);
    return 0;
}

static void client(int fd, const unsigned char *frame, size_t n) {
    SSL_CTX *c = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);
    SSL *s = SSL_new(c);
    SSL_set_fd(s, fd);
    if (SSL_connect(s) != 1) _exit(3);
    static const char pre[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    SSL_write(s, pre, (int)(sizeof pre - 1));
    unsigned char in[256];
    if (SSL_read(s, in, 9) <= 0) _exit(6);        /* the server's SETTINGS: it is in the loop */
    SSL_write(s, frame, (int)n);
    shutdown(fd, SHUT_WR);             /* no close_notify, no more bytes: the payload never comes */
    while (read(fd, in, sizeof in) > 0) {}       /* let the server's writes land */
    _exit(0);
}

/* 0 = handle_h2_connection returned; otherwise how the server died. */
static int run(const unsigned char *frame, size_t n, char *how, size_t hl) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { snprintf(how, hl, "socketpair"); return -1; }
    pid_t cp = fork();
    if (cp == 0) { close(sv[0]); client(sv[1], frame, n); }
    close(sv[1]);
    pid_t sp = fork();
    if (sp == 0) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx || make_cert(ctx) != 0) _exit(4);
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sv[0]);
        if (SSL_accept(ssl) != 1) _exit(5);
        handle_h2_connection(sv[0], ssl, "127.0.0.1");
        _exit(0);
    }
    close(sv[0]);
    int st = 0, cst = 0;
    waitpid(sp, &st, 0);
    waitpid(cp, &cst, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { snprintf(how, hl, "returned"); return 0; }
    if (WIFSIGNALED(st)) snprintf(how, hl, "killed by signal %d", WTERMSIG(st));
    else snprintf(how, hl, "exit %d", WEXITSTATUS(st));
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);          /* as http_serve does: a write to a gone peer is EPIPE */
    int fails = 0; char how[64];
    /* PING, 8 bytes, complete: the control. */
    static const unsigned char ping[] = { 0,0,8, 6, 0, 0,0,0,0, 1,2,3,4,5,6,7,8 };
    int r = run(ping, sizeof ping, how, sizeof how);
    printf("  %s: control: a complete PING then close -> server %s\n", r == 0 ? "PASS" : "FAIL", how);
    fails += r != 0;
    /* HEADERS on stream 1, END_HEADERS, length 10 -- and no payload. */
    static const unsigned char hdr[] = { 0,0,10, 1, 4, 0,0,0,1 };
    r = run(hdr, sizeof hdr, how, sizeof how);
    printf("  %s: HEADERS header announcing 10 bytes, then close -> server %s\n", r == 0 ? "PASS" : "FAIL", how);
    fails += r != 0;
    /* WINDOW_UPDATE, length 4 -- and no payload. */
    static const unsigned char wu[] = { 0,0,4, 8, 0, 0,0,0,0 };
    r = run(wu, sizeof wu, how, sizeof how);
    printf("  %s: WINDOW_UPDATE header announcing 4 bytes, then close -> server %s\n", r == 0 ? "PASS" : "FAIL", how);
    fails += r != 0;
    printf("\nResults: %d passed, %d failed\n", 3 - fails, fails);
    return fails ? 1 : 0;
}
C

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
# http.c is #included into t.c above, so it is left out of the set.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http.c http)" \
    || { echo "  FAIL: stdlib link set: see stderr above"; echo "C209: FAIL (build)"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# shellcheck disable=SC2086
if ! "${CC}" -std=c11 -D_GNU_SOURCE -DTK_HAVE_OPENSSL -g -O1 ${SAN} -iquote "${S}" ${OSSL_CFLAGS} \
        -Wno-deprecated-declarations -Wno-unused-function -o "${WORK}/t" "${WORK}/t.c" \
        "${LINK[@]}" ${OSSL_LIBS} -lz -lm -lpthread >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    echo "C209: FAIL (build)"
    exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" 2>"${WORK}/err.log"
rc=$?
awk '/ERROR: AddressSanitizer|SUMMARY|runtime error/' "${WORK}/err.log" | head -4 | sed 's/^/    /'
echo ""
[ "${rc}" -eq 0 ] && echo "C209: PASS" || echo "C209: FAIL (exit ${rc})"
exit "${rc}"
