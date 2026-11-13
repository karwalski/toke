#!/usr/bin/env bash
# C1550_https_server_peer_close_no_sigpipe.sh — std.http's https server
# (handle_tls_connection, reached by http.servetls) writes its TLS records
# through send(MSG_NOSIGNAL), so a client that sent its ClientHello and left
# before accept() returned is an error, never SIGPIPE (story 127.394).
#
# THE DEFECT.  handle_tls_connection set SO_NOSIGPIPE on the accepted socket
# (127.144) and then attached it with plain SSL_set_fd, whose socket BIO
# writes with write(2).  On Darwin setsockopt(SO_NOSIGPIPE) fails with EINVAL
# once the peer has closed, so for a client that is already gone the option
# was never set and the ServerHello write killed the server with status 141.
# (On Linux the process-wide SIG_IGN in http_serve_tls_workers (127.144,
# owner §3.6) covers the same write; it is left as it is.)  The fix attaches
# the socket with tk_ssl_set_fd_nosigpipe() (src/stdlib/tk_ssl_nosigpipe.h,
# 127.346), as tls.c's accept path does (127.351): OpenSSL's socket BIO for
# reading, a write BIO that calls send(fd, ..., MSG_NOSIGNAL).
#
# WHY IT IS DETERMINISTIC (the C1491 'listen' pattern: socketpair, closed
# peer, no clock).  The probe links the real http.c and defines its own
# accept(), which the static link binds http.c's call to.  The first call
# returns one end of a socketpair whose other end has written a real
# ClientHello (made by an in-memory OpenSSL client) and then closed, so the
# server reads the hello and its ServerHello write meets a peer that is gone.
# The second call waits for handle_tls_connection to close that socket
# (bounded: 10 s), then reports and exits.  SIGPIPE is at its default
# disposition in the probe.
#   control — the old sequence by hand: SO_NOSIGPIPE as http.c sets it (on
#             Darwin it is refused, EINVAL), plain SSL_set_fd, SSL_accept on
#             the same closed peer — killed (rc 141), so a probe that cannot
#             observe SIGPIPE at all cannot pass.
#   serve   — http_serve_tls_workers(..., 1) through the real
#             tls_worker_loop and handle_tls_connection: the handshake fails,
#             the socket is closed and the server lives.
#
# Negative control: C1550_STDLIB=<dir holding dd3a51e1's src/stdlib> — the
# serve case is killed (rc 141); see the commit.
#
# Needs OpenSSL headers and the openssl CLI (for the server certificate).
#
# Story: 127.394

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SL="${C1550_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

OSSL_CFLAGS=""; OSSL_LIBS=""
if pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in /opt/homebrew /opt/homebrew/opt/openssl /usr/local/opt/openssl; do
        if [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
OPENSSL_BIN=""
for c in /opt/homebrew/bin/openssl /opt/homebrew/opt/openssl/bin/openssl /usr/local/opt/openssl/bin/openssl openssl; do
    if command -v "$c" >/dev/null 2>&1; then OPENSSL_BIN="$c"; break; fi
done
if [ -z "${OSSL_LIBS}" ] || [ -z "${OPENSSL_BIN}" ]; then
    echo "CONFORM-SKIP: OpenSSL not installed"; exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1550_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

if ! "${OPENSSL_BIN}" req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 2 \
        -subj "/CN=localhost" -keyout k.pem -out c.pem > cert.log 2>&1 || [ ! -s c.pem ]; then
    echo "FAIL: test certificate: $(tail -2 cert.log)"; echo "C1550: 0 passed, 1 failed"; exit 1
fi

cat > probe.c <<'C'
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include "http.h"
#include "capabilities.h"

static size_t client_hello(unsigned char *out, size_t cap)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = ctx ? SSL_new(ctx) : NULL;
    if (!ssl) return 0;
    BIO *rb = BIO_new(BIO_s_mem()), *wb = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl, rb, wb);
    SSL_set_connect_state(ssl);
    (void)SSL_do_handshake(ssl);              /* WANT_READ, hello in wb */
    int n = BIO_read(wb, out, (int)cap);
    SSL_free(ssl); SSL_CTX_free(ctx);
    return n > 0 ? (size_t)n : 0;
}

/* One end of a socketpair holding a ClientHello whose sender has left. */
static int closed_peer_with_hello(void)
{
    unsigned char hello[16384];
    int sv[2];
    size_t n = client_hello(hello, sizeof hello);
    if (n == 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { printf("setup failed\n"); _exit(5); }
    if (write(sv[1], hello, n) != (ssize_t)n) { printf("setup failed\n"); _exit(5); }
    close(sv[1]);
    return sv[0];
}

static int naccept, first_fd = -1;
/* tls_worker_loop's accept(): a client that sent its hello and left. */
int accept(int lfd, struct sockaddr *a, socklen_t *l)
{
    (void)lfd; (void)a; (void)l;
    if (naccept++ == 0) {
        first_fd = closed_peer_with_hello();
        return first_fd;
    }
    /* Wait (bounded) for handle_tls_connection to close the socket. */
    struct timespec ms = { 0, 1000000 };
    int closed = 0;
    for (int i = 0; i < 10000 && !closed; i++) {
        if (fcntl(first_fd, F_GETFD) == -1 && errno == EBADF) closed = 1;
        else nanosleep(&ms, NULL);
    }
    printf("survived closed=%d\n", closed);
    fflush(stdout);
    _exit(0);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "control";
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_DFL);
    tk_cap_init(argc, argv);
    if (argc < 4) return 2;
    if (strcmp(mode, "control") == 0) {
        SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
        if (!ctx || SSL_CTX_use_certificate_chain_file(ctx, argv[2]) != 1
            || SSL_CTX_use_PrivateKey_file(ctx, argv[3], SSL_FILETYPE_PEM) != 1) return 3;
        int fd = closed_peer_with_hello();
        http_sock_nosigpipe(fd);              /* what handle_tls_connection did */
        SSL *ssl = SSL_new(ctx);
        if (!ssl || SSL_set_fd(ssl, fd) != 1) return 3;
        int r = SSL_accept(ssl);
        printf("survived %d\n", r);
        return 0;
    }
    if (strcmp(mode, "serve") == 0) {
        TkTlsCtx *t = http_tls_ctx_new(argv[2], argv[3]);
        if (!t) { printf("ctx failed\n"); return 4; }
        TkHttpErr e = http_serve_tls_workers(NULL, "127.0.0.1", 0, t, 1);
        printf("serve returned %d\n", (int)e);   /* exits from the 2nd accept() */
        return 6;
    }
    return 2;
}
C

CF=(-std=c11 -D_GNU_SOURCE -DTK_HAVE_OPENSSL -Wall -Wextra -Werror -Wno-deprecated-declarations -iquote "${SL}" ${OSSL_CFLAGS})
# The link set comes from scripts/stdlib_link_set.sh as cached objects (C696, C703).
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${SL}" --tkc "${TKC:-${REPO_ROOT}/tkc}" --cc "${CC}" \
           --objects http -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set"; echo "C1550: 0 passed, 1 failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o probe probe.c "${LS[@]}" ${OSSL_LIBS} -lz -lpthread > build.log 2>&1; then
    echo "FAIL: the probe does not build"; head -8 build.log; echo "C1550: 0 passed, 1 failed"; exit 1
fi
# stdout only: the server's own stderr lines (DIAGNOSTIC: server_listening ...) go to a log.
run() { out="$(trap - PIPE; ./probe "$1" c.pem k.pem --allow-net 2> "$1.err" | head -c 4000; exit "${PIPESTATUS[0]}")"; rc=$?; }

run control
expect "0-control-SO_NOSIGPIPE-then-SSL_set_fd-on-a-closed-peer-is-killed-by-SIGPIPE" "rc=${rc}" "rc=141"
run serve
expect "1-servetls-ServerHello-to-a-client-that-left-is-not-SIGPIPE" "rc=${rc} ${out}" "rc=0 survived closed=1"

echo "C1550: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
