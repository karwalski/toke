#!/usr/bin/env bash
# C206_tls_negotiated_group.sh — std.tls reports the key-exchange group each
# connection negotiated, so a hybrid -> classical fallback is observable
# (story 122.13 (6)).
#
# THE GAP (on main before 122.13).  build_ssl_ctx prefers the post-quantum
# hybrid X25519MLKEM768 and keeps classical groups for interop; with an
# OpenSSL older than 3.5, or a peer that does not offer the hybrid, the
# handshake settles on a classical group.  Nothing reported it and no API
# could ask, so "is this connection quantum-resistant?" had no answer from
# toke or from C.  tls.group(conn) / tls_group() is the additive answer.
#
# WHAT THIS DRIVES (ASan+UBSan build of tls.c against the host OpenSSL):
#   a std.tls server (tls_listen, self-signed P-384 cert) records
#   tls_group() for every accepted connection;
#   1. a std.tls client (tls_connect): both ends report the same group, and
#      on OpenSSL >= 3.5 it is X25519MLKEM768;
#   2. an independent OpenSSL client restricted to classical X25519: the
#      server reports "x25519" -- the fallback is visible, not silent;
#   3. tls_group on an unknown handle returns "" (not a crash).
# Also: tls.group is declared in stdlib/tls.tki and tk_tls_group_w exists.
#
# Negative control: C206_STDLIB=<dir> builds against another stdlib tree; with
# main's tls.c/tls.h there is no tls_group and the driver does not build.
#
# Needs OpenSSL headers; without them the test is a named SKIP.
#
# Story: 122.13

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C206_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c206_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C206: tls.group names the negotiated key-exchange group"
echo "-------------------------------------------------------"

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

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if grep -q '"name": "tls.group"' "${REPO_ROOT}/stdlib/tls.tki" \
   && grep -q 'int64_t tk_tls_group_w(int64_t conn)' "${S}/tls_glue.c"; then
    pass "tls.group is declared in stdlib/tls.tki and has its tk_tls_group_w wrapper"
else
    fail "tls.group missing from stdlib/tls.tki or tls_glue.c"
fi

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/opensslv.h>
#include "tls.h"
#include "capabilities.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static char srv_group[4][64];
static int nsrv = 0;
static void on_conn(TlsConn c) {
    char *g = tls_group(c);
    pthread_mutex_lock(&mu);
    if (nsrv < 4) snprintf(srv_group[nsrv++], 64, "%s", g ? g : "(null)");
    pthread_mutex_unlock(&mu);
    free(g);
    tls_close(c);
}
static TlsConfig scfg;
static int port;
static void *srv(void *a) { (void)a; tls_listen(port, scfg, on_conn); return NULL; }
static const char *wait_srv(int i) {
    for (int k = 0; k < 200; k++) {
        pthread_mutex_lock(&mu); int n = nsrv; pthread_mutex_unlock(&mu);
        if (n > i) return srv_group[i];
        usleep(10000);
    }
    return "(no server connection)";
}
static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    tk_cap_init(argc, argv);
    TlsKeypairResult kp = tls_gen_self_signed("localhost", 1);
    if (kp.is_err) { printf("  FAIL: keygen: %s\n", kp.err_msg); return 1; }
    scfg.cert_pem = kp.cert_pem; scfg.key_pem = kp.key_pem;
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    bind(probe, (struct sockaddr *)&a, sizeof a); getsockname(probe, (struct sockaddr *)&a, &al);
    port = ntohs(a.sin_port); close(probe);
    pthread_t t; pthread_create(&t, NULL, srv, NULL); pthread_detach(t);

    /* 1. std.tls client */
    TlsConfig ccfg; memset(&ccfg, 0, sizeof ccfg);
    TlsConnResult r = { {0}, 1 };
    for (int k = 0; k < 100 && r.is_none; k++) { r = tls_connect("127.0.0.1", port, ccfg); if (r.is_none) usleep(20000); }
    if (r.is_none) { printf("  FAIL: tls_connect never succeeded\n"); return 1; }
    char *cg = tls_group(r.conn);
    const char *sg = wait_srv(0);
    printf("  info: std.tls client <-> server negotiated %s (OpenSSL %s)\n", cg, OPENSSL_VERSION_TEXT);
    CHECK(cg[0] && strcmp(cg, sg) == 0, "client and server report the same, non-empty group");
#if OPENSSL_VERSION_NUMBER >= 0x30500000L
    CHECK(strcmp(cg, "X25519MLKEM768") == 0, "OpenSSL >= 3.5 on both ends: the hybrid X25519MLKEM768 is negotiated");
#else
    printf("  SKIP: OpenSSL < 3.5 -- the hybrid group is unavailable here, so only the fallback is checked\n");
#endif
    free(cg);
    tls_close(r.conn);

    /* 2. an independent client that offers only classical X25519 */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set1_groups_list(ctx, "X25519");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    a.sin_port = htons((uint16_t)port);
    int ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    SSL *ssl = SSL_new(ctx); SSL_set_fd(ssl, fd);
    ok = ok && SSL_connect(ssl) == 1;
    CHECK(ok, "a classical-only OpenSSL client completes the handshake (the fallback path)");
    sg = wait_srv(1);
    printf("  info: server saw %s for the classical-only client\n", sg);
    CHECK(strcasecmp(sg, "x25519") == 0, "the server reports the classical fallback group (x25519), not silence");
    SSL_shutdown(ssl); SSL_free(ssl); close(fd); SSL_CTX_free(ctx);

    /* 3. unknown handle */
    TlsConn bogus = { (char *)"no-such-connection" };
    char *bg = tls_group(bogus);
    CHECK(bg && bg[0] == '\0', "tls_group on an unknown handle returns an empty string");
    free(bg);

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" tls)" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -DTK_HAVE_OPENSSL -g -O1 ${SAN} -Wno-deprecated-declarations \
        -iquote "${S}" ${OSSL_CFLAGS} -o "${WORK}/t" "${WORK}/t.c" "${LINK[@]}" \
        ${OSSL_LIBS} -lpthread >"${WORK}/cc.log" 2>&1; then
    fail "driver does not build against ${S}: $(grep -a -m1 -E 'error' "${WORK}/cc.log")"
    echo ""; echo "C206: ${PASS} passed, ${FAIL} failed"; exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" --allow-net 2>"${WORK}/err.log"
rc=$?
grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/err.log" | sed 's/^/  /'
[ "${rc}" -eq 0 ] || FAIL=$((FAIL + 1))
# The toke surface: a program naming tls.group type-checks, links against
# tk_tls_group_w and runs (a closed/zero handle reads back as "").
TKC="${TKC:-${REPO_ROOT}/tkc}"
if [ -x "${TKC}" ] && [ -z "${C206_STDLIB:-}" ]; then
    cat > "${WORK}/g.tk" <<'EOF'
m=app;
i=tls:std.tls;
i=io:std.io;
i=str:std.str;
f=main():i64{
  io.println(str.concat("g=[";str.concat(tls.group(0);"]")));
  <0
};
EOF
    if (cd "${WORK}" && "${TKC}" --allow-all -o g.bin g.tk >g.build 2>&1) \
       && [ "$("${WORK}/g.bin" 2>&1)" = "g=[]" ]; then
        pass "a toke program calling tls.group compiles, links and runs (zero handle -> \"\")"
    else
        fail "toke program calling tls.group: $(head -c 300 "${WORK}/g.build" 2>/dev/null)"
    fi
else
    echo "  SKIP: toke-surface check (no tkc at ${TKC}, or a C206_STDLIB override is set)"
fi

echo ""
echo "C206: ${PASS} + driver ${rc} (0 = all driver checks passed)"
[ "${FAIL}" -eq 0 ] && [ "${rc}" -eq 0 ]
