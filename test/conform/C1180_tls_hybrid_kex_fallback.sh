#!/usr/bin/env bash
# C1180_tls_hybrid_kex_fallback.sh — std.tls offers the post-quantum hybrid
# X25519MLKEM768 when the linked OpenSSL has it, and falls back CLEANLY to
# classical groups when it does not (story 122.5, ADR-0013 (c)).
#
# THE GAP.  build_ssl_ctx (tls.c) tries "X25519MLKEM768:X25519:..." and, if
# OpenSSL rejects that list, retries with the classical list.  C206 proves the
# hybrid is negotiated and that a classical-only PEER is served; nothing drove
# the other branch -- a LIBRARY that does not know the hybrid (OpenSSL < 3.5)
# -- because every OpenSSL on this host has it.  That branch had never run.
#
# HOW THE ABSENT LIBRARY IS SIMULATED.  The two group lists are macros in
# tls.h (TK_TLS_GROUPS_HYBRID / TK_TLS_GROUPS_CLASSICAL).  OpenSSL rejects a
# whole groups list when one name in it is unknown, which is exactly what an
# OpenSSL < 3.5 does with "X25519MLKEM768".  So tls.c is compiled three ways:
#
#   A. as shipped: the linked library must ACCEPT the hybrid list (the build-
#      time availability check for Homebrew OpenSSL 3.5+/4.x), and a std.tls
#      client <-> std.tls server handshake negotiates X25519MLKEM768;
#   B. hybrid list names a group this library does not know: the contexts
#      still build, the handshake completes, both ends report x25519, and a
#      payload round-trips -- the fallback is clean, not a failure;
#   C. negative control: BOTH lists unknown.  tls_connect must fail closed
#      (is_none), proving B passes because the fallback ran, not because the
#      library silently ignores unknown names.
#
# Plus a drift guard: the classical list is the hybrid list minus its first
# (hybrid) entry, so the fallback never silently drops a classical group.
#
# Needs OpenSSL headers; without them the test is a named SKIP.
#
# Story: 122.5

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1180_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1180: std.tls hybrid KEX present, and a clean classical fallback when absent"
echo "-----------------------------------------------------------------------------"

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
    echo "SKIP: no OpenSSL headers on this host"
    echo "CONFORM-SKIP: OpenSSL not installed"
    exit 0
fi

PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# ── drift guard on the two lists ────────────────────────────────────────────
HYB="$(sed -n 's/^#define TK_TLS_GROUPS_HYBRID *"\(.*\)"$/\1/p' "${S}/tls.h")"
CLA="$(sed -n 's/^#define TK_TLS_GROUPS_CLASSICAL *"\(.*\)"$/\1/p' "${S}/tls.h")"
if [ "${HYB%%:*}" = "X25519MLKEM768" ] && [ "${HYB#*:}" = "${CLA}" ] && [ -n "${CLA}" ]; then
    pass "hybrid list leads with X25519MLKEM768 and the fallback is the same list without it (${CLA})"
else
    fail "group lists drifted: hybrid='${HYB}' classical='${CLA}'"
fi
if grep -q 'SSL_CTX_set1_groups_list(ctx, TK_TLS_GROUPS_HYBRID)' "${S}/tls.c" \
   && grep -q 'SSL_CTX_set1_groups_list(ctx, TK_TLS_GROUPS_CLASSICAL)' "${S}/tls.c"; then
    pass "build_ssl_ctx sets the hybrid list, then the classical list on rejection"
else
    fail "build_ssl_ctx no longer sets TK_TLS_GROUPS_HYBRID / TK_TLS_GROUPS_CLASSICAL"
fi

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
static char srv_group[64];
static char srv_msg[64];
static int nsrv = 0;
static void on_conn(TlsConn c) {
    char *g = tls_group(c);
    TlsStrResult r = tls_read(c);
    pthread_mutex_lock(&mu);
    snprintf(srv_group, sizeof srv_group, "%s", g ? g : "(null)");
    snprintf(srv_msg, sizeof srv_msg, "%s", (!r.is_none && r.ok) ? r.ok : "(none)");
    nsrv++;
    pthread_mutex_unlock(&mu);
    free(g);
    tls_close(c);
}
static TlsConfig scfg;
static int port;
static void *srv(void *a) { (void)a; tls_listen(port, scfg, on_conn); return NULL; }
static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *expect = getenv("C1180_EXPECT");   /* hybrid | classical | fail */
    if (!expect) expect = "hybrid";
    tk_cap_init(argc, argv);
    printf("  info: [%s] TK_TLS_GROUPS_HYBRID=\"%s\" against %s\n",
           expect, TK_TLS_GROUPS_HYBRID, OPENSSL_VERSION_TEXT);

    /* Is the hybrid list (as compiled into tls.c) usable on this library? */
    SSL_CTX *probe = SSL_CTX_new(TLS_client_method());
    int lib_has_hybrid = probe && SSL_CTX_set1_groups_list(probe, TK_TLS_GROUPS_HYBRID) == 1;
    SSL_CTX_free(probe);
    if (strcmp(expect, "hybrid") == 0)
        CHECK(lib_has_hybrid, "the linked OpenSSL accepts X25519MLKEM768 (hybrid KEX available at build)");
    else
        CHECK(!lib_has_hybrid, "the simulated hybrid list is rejected by this library (absence simulated)");

    TlsKeypairResult kp = tls_gen_self_signed("localhost", 1);
    if (kp.is_err) { printf("  FAIL: keygen: %s\n", kp.err_msg); return 1; }
    scfg.cert_pem = kp.cert_pem; scfg.key_pem = kp.key_pem;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    bind(s, (struct sockaddr *)&a, sizeof a); getsockname(s, (struct sockaddr *)&a, &al);
    port = ntohs(a.sin_port); close(s);
    pthread_t t; pthread_create(&t, NULL, srv, NULL); pthread_detach(t);

    TlsConfig ccfg; memset(&ccfg, 0, sizeof ccfg);
    ccfg.peer_cert_pem = kp.cert_pem;          /* pinned: no trust-store lookup */
    TlsConnResult r = { {0}, 1 };
    int tries = strcmp(expect, "fail") == 0 ? 10 : 100;
    for (int k = 0; k < tries && r.is_none; k++) {
        r = tls_connect("127.0.0.1", port, ccfg);
        if (r.is_none) usleep(20000);
    }

    if (strcmp(expect, "fail") == 0) {
        CHECK(r.is_none, "with no usable group list at all, tls_connect fails closed (is_none, no crash)");
        if (!r.is_none) tls_close(r.conn);
    } else if (r.is_none) {
        CHECK(0, "tls_connect completed a handshake");
    } else {
        CHECK(1, "tls_connect completed a handshake");
        char *cg = tls_group(r.conn);
        CHECK(tls_write(r.conn, "c1180-ping") > 0, "client wrote a payload over the connection");
        const char *sg = "(no server connection)", *sm = "";
        for (int k = 0; k < 200; k++) {
            pthread_mutex_lock(&mu); int n = nsrv; pthread_mutex_unlock(&mu);
            if (n > 0) { sg = srv_group; sm = srv_msg; break; }
            usleep(10000);
        }
        printf("  info: client reports %s, server reports %s\n", cg, sg);
        CHECK(strcmp(cg, sg) == 0, "client and server report the same group");
        CHECK(strcmp(sm, "c1180-ping") == 0, "the server received the payload intact");
        if (strcmp(expect, "hybrid") == 0)
            CHECK(strcmp(cg, "X25519MLKEM768") == 0, "the hybrid X25519MLKEM768 was negotiated");
        else
            CHECK(strcasecmp(cg, "x25519") == 0, "the fallback negotiated classical x25519");
        free(cg);
        tls_close(r.conn);
    }
    printf("  Results: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

CF=(-std=c11 -D_GNU_SOURCE -DTK_HAVE_OPENSSL -g -O1 -Wno-deprecated-declarations -iquote "${S}" ${OSSL_CFLAGS})
# Everything std.tls links except tls.c, which each variant compiles itself.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" \
           --except tls.c --objects tls -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "C1180: ${PASS} passed, $((FAIL + 1)) failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

run_variant() {   # name expect [extra -D flags...]
    local name="$1" expect="$2"; shift 2
    echo "  -- variant ${name} (expect ${expect})"
    if ! "${CC}" "${CF[@]}" "$@" -o "${WORK}/t_${name}" "${WORK}/t.c" "${S}/tls.c" "${LINK[@]}" \
            ${OSSL_LIBS} -lpthread >"${WORK}/cc_${name}.log" 2>&1; then
        fail "variant ${name}: driver does not build: $(grep -a -m1 -E 'error' "${WORK}/cc_${name}.log")"
        return
    fi
    if C1180_EXPECT="${expect}" "${WORK}/t_${name}" --allow-net; then
        pass "variant ${name}: all driver checks passed"
    else
        fail "variant ${name}: driver checks failed"
    fi
}

run_variant A_shipped hybrid
run_variant B_hybrid_absent classical \
    "-DTK_TLS_GROUPS_HYBRID=\"TkC1180NoSuchHybrid:${CLA}\""
run_variant C_nothing_usable fail \
    "-DTK_TLS_GROUPS_HYBRID=\"TkC1180NoSuchHybrid:TkC1180NoSuchClassical\"" \
    "-DTK_TLS_GROUPS_CLASSICAL=\"TkC1180NoSuchClassical\""

echo ""
echo "C1180: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
