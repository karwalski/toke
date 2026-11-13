#!/usr/bin/env bash
# C1491_tls_llm_peer_close_no_sigpipe.sh — std.tls (server accept and client
# connect) and std.llm's https client write TLS records through
# send(MSG_NOSIGNAL), so a peer that has already closed is an error, never
# SIGPIPE (story 127.351).
#
# THE DEFECT.  tls_listen(), tls_connect() and llm.c's http_exchange() handed
# their sockets to OpenSSL with SSL_set_fd, whose socket BIO writes with
# write(2).  None of those sockets had SO_NOSIGPIPE and OpenSSL never passes
# MSG_NOSIGNAL, so on every platform a peer that closed first killed the toke
# program with status 141.  The fix attaches each socket with
# tk_ssl_set_fd_nosigpipe() (src/stdlib/tk_ssl_nosigpipe.h, 127.346):
# OpenSSL's socket BIO for reading, a write BIO that calls
# send(fd, ..., MSG_NOSIGNAL).
#
# WHY IT IS DETERMINISTIC (the C1301 Part A pattern: socketpair, closed peer,
# no clock).  The probe links the real tls.c and llm.c and defines its own
# connect() and accept(), which the static link binds those files' calls to:
#   connect — dup2()s one end of a fresh AF_UNIX socketpair over the TCP
#             socket and closes the other end, so the ClientHello write meets
#             a peer that is gone (EPIPE, every time).
#   accept  — returns one end of a socketpair whose other end has written a
#             real ClientHello (made by an in-memory OpenSSL client) and then
#             closed, so the server reads the hello and its ServerHello write
#             meets a peer that is gone.  The second accept() call waits for
#             the connection thread to close that socket (bounded: 10 s), then
#             reports and exits.
# SIGPIPE is at its default disposition in the probe.
#   control — plain SSL_set_fd on a closed socketpair is killed (rc 141), so a
#             probe that cannot observe SIGPIPE at all cannot pass.
#   connect — tls_connect() returns none and the probe lives.
#   listen  — tls_listen()'s connection thread fails the handshake and the
#             probe lives.
#   llm     — llm_chat() over https:// is an error and the probe lives (two
#             attempts: max_retries 1).
#
# Negative control: C1491_STDLIB=<dir holding e31aac45's src/stdlib> — the
# connect, listen and llm cases are killed (rc 141); see the commit.
#
# Story: 127.351

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SL="${C1491_STDLIB:-${REPO_ROOT}/src/stdlib}"
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
if [ -z "${OSSL_LIBS}" ]; then
    echo "CONFORM-SKIP: OpenSSL not installed"; exit 0
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1491_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

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
#include "tls.h"
#include "llm.h"
#include "capabilities.h"

static int nconnect;
/* Every connect() made by tls.c / llm.c lands here: the "server" is gone. */
int connect(int fd, const struct sockaddr *a, socklen_t l)
{
    int sv[2];
    (void)a; (void)l;
    nconnect++;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (dup2(sv[0], fd) < 0) return -1;
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int naccept, first_fd = -1;
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
/* tls_listen's accept(): a client that sent its hello and left. */
int accept(int lfd, struct sockaddr *a, socklen_t *l)
{
    (void)lfd; (void)a; (void)l;
    if (naccept++ == 0) {
        unsigned char hello[16384];
        int sv[2];
        size_t n = client_hello(hello, sizeof hello);
        if (n == 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { printf("setup failed\n"); _exit(5); }
        if (write(sv[1], hello, n) != (ssize_t)n) { printf("setup failed\n"); _exit(5); }
        close(sv[1]);
        first_fd = sv[0];
        return sv[0];
    }
    /* Wait (bounded) for the connection thread to close the socket. */
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
    if (strcmp(mode, "control") == 0) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
        close(sv[1]);
        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        SSL *ssl = ctx ? SSL_new(ctx) : NULL;
        if (!ssl || SSL_set_fd(ssl, sv[0]) != 1) return 3;
        int r = SSL_connect(ssl);
        printf("survived %d\n", r);
        return 0;
    }
    if (strcmp(mode, "connect") == 0) {
        TlsConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        TlsConnResult r = tls_connect("127.0.0.1", 9, cfg);
        printf("survived none=%d connects=%d\n", r.is_none, nconnect);
        return 0;
    }
    if (strcmp(mode, "listen") == 0) {
        TlsKeypairResult kp = tls_gen_self_signed("localhost", 2);
        if (kp.is_err) { printf("keygen failed\n"); return 4; }
        TlsConfig cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.cert_pem = kp.cert_pem;
        cfg.key_pem  = kp.key_pem;
        tls_listen(0, cfg, NULL);             /* exits from the 2nd accept() */
        printf("tls_listen returned\n");
        return 6;
    }
    if (strcmp(mode, "llm") == 0) {
        TkLlmClient *c = llm_client("https://127.0.0.1:9/v1", "", "m");
        if (!c) return 7;
        c->max_retries = 1;
        TkLlmMsg m = { "user", "hi" };
        TkLlmResp r = llm_chat(c, &m, 1, 0.0);
        printf("survived err=%d connects=%d\n", r.is_err, nconnect);
        return 0;
    }
    return 2;
}
C

CF=(-std=c11 -D_GNU_SOURCE -DTK_HAVE_OPENSSL -Wall -Wextra -Werror -Wno-deprecated-declarations -iquote "${SL}" ${OSSL_CFLAGS})
# The link set comes from scripts/stdlib_link_set.sh as cached objects (C696, C703).
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${SL}" --tkc "${TKC:-${REPO_ROOT}/tkc}" --cc "${CC}" \
           --objects tls llm -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set"; echo "C1491: 0 passed, 1 failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o probe probe.c "${LS[@]}" ${OSSL_LIBS} -lz -lpthread > build.log 2>&1; then
    echo "FAIL: the probe does not build"; head -8 build.log; echo "C1491: 0 passed, 1 failed"; exit 1
fi
run() { out="$(trap - PIPE; ./probe "$1" --allow-net 2>&1 | head -c 4000; exit "${PIPESTATUS[0]}")"; rc=$?; }

run control
expect "0-control-SSL_set_fd-on-a-closed-peer-is-killed-by-SIGPIPE" "rc=${rc}" "rc=141"
run connect
expect "1-tls_connect-to-a-closed-peer-is-none-not-SIGPIPE"         "rc=${rc} ${out}" "rc=0 survived none=1 connects=1"
run listen
expect "2-tls_listen-ServerHello-to-a-closed-peer-is-not-SIGPIPE"   "rc=${rc} ${out}" "rc=0 survived closed=1"
run llm
expect "3-llm-https-ClientHello-to-a-closed-peer-is-err-not-SIGPIPE" "rc=${rc} ${out}" "rc=0 survived err=1 connects=2"

echo "C1491: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
