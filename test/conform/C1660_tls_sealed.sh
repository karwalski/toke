#!/usr/bin/env bash
# C1660_tls_sealed.sh -- std.tls hands toke sealed strings, and tls.c reads
# with tk_strlen (story 127.295, S1 step 5 tranche 15; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen.  tls.c's producers (registry
# ids, tls_read, tls_peer_cert, tls_fingerprint, tls_pairing_code, tls_group,
# tls_protocol) stay malloc'd because their C callers (test_tls.c, C206,
# C590, C1180) free them, so tls_glue.c seals at the boundary (seal_take:
# sealed copy, original freed).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), over the glue the compiler calls and a real
#      TLS 1.3 loopback connection (tls.listen in a thread, tls.connect):
#        - tls.certof: a sealed copy equal to the keypair's PEM; tls.keyof
#          stays the keypair's own malloc'd buffer (a sealed copy of a
#          private key could never be wiped);
#        - tls.fingerprint: sealed, 64 hex chars, equal to tls_fingerprint
#          of the unsealed PEM; of "" a sealed "";
#        - client: the $TlsConn .id, tls.read ("pong"), tls.peercert (the
#          server's certificate: its fingerprint matches), tls.protocol
#          ("TLSv1.3"), tls.group and tls.pairingcode ("000000": the client
#          has no local certificate) are all sealed;
#        - server handler: the $TlsConn .id it receives, tls.read ("ping"),
#          tls.protocol, tls.group and tls.pairingcode sealed;
#        - the reader: tls.write of a sealed string on both ends delivers
#          exactly its bytes (the other side reads them back);
#        - tls.group of no connection: a sealed "".
#   2. toke, under TK_STR_VERIFY=1: tls.certof / tls.fingerprint and their
#      .len, tls.group of no connection.
#
# Negative control: C1660_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base 75ec5f49's src/stdlib + stdlib: 1 passed, 8 failed -- only the toke
#   case passes there (an unsealed string read with strlen was always
#   correct); every producer case reports "not sealed", and the tls.write
#   case with them, because it also checks that tls.read's result is sealed.
# Measured: this branch 9 passed (8 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1660_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1660_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1660: std.tls results sealed; tls.c reads with tk_strlen (127.295)"
echo "---------------------------------------------------------------------"

# OpenSSL, found as C206 finds it.
OSSL_CFLAGS=""; OSSL_LIBS=""
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 /opt/homebrew /usr/local; do
        if [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
if [ -z "${OSSL_LIBS}" ]; then
    echo "CONFORM-SKIP: OpenSSL headers/libraries not found"
    exit 0
fi

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "tk_strhdr.h"
#include "tls.h"
#include "capabilities.h"
int64_t tk_tls_genselfsigned_w(int64_t cn, int64_t days);
int64_t tk_tls_certof_w(int64_t kp);
int64_t tk_tls_keyof_w(int64_t kp);
int64_t tk_tls_tlsconfig_w(int64_t cert, int64_t key);
int64_t tk_tls_pinconfig_w(int64_t cert, int64_t key, int64_t peer);
int64_t tk_tls_fingerprint_w(int64_t pem);
int64_t tk_tls_listen_w(int64_t port, int64_t cfg, int64_t handler);
int64_t tk_tls_connect_w(int64_t host, int64_t port, int64_t cfg);
int64_t tk_tls_read_w(int64_t conn);
int64_t tk_tls_write_w(int64_t conn, int64_t data);
int64_t tk_tls_close_w(int64_t conn);
int64_t tk_tls_peercert_w(int64_t conn);
int64_t tk_tls_pairingcode_w(int64_t conn);
int64_t tk_tls_group_w(int64_t conn);
int64_t tk_tls_protocol_w(int64_t conn);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.40s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%.40s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
static int sealed_nonempty(const char *s) { return sealed_eq(s, NULL) && s[0]; }
static const char *conn_id(int64_t rec) { return rec ? ((const TlsConn *)(intptr_t)rec)->id : NULL; }

/* The server handler, as tls.listen calls a toke handler: one $TlsConn. */
static volatile int srv_done = 0, srv_ok = 0;
static int64_t on_conn(int64_t rec)
{
    int ok = sealed_nonempty(conn_id(rec));
    ok = sealed_eq(P(tk_tls_read_w(rec)), "ping") && ok;
    ok = sealed_eq(P(tk_tls_protocol_w(rec)), "TLSv1.3") && ok;
    ok = sealed_nonempty(P(tk_tls_group_w(rec))) && ok;
    ok = sealed_eq(P(tk_tls_pairingcode_w(rec)), "000000") && ok;   /* no client cert */
    ok = tk_tls_write_w(rec, I(tks_strdup("pong"))) == 1 && ok;
    srv_ok = ok;
    srv_done = 1;
    tk_tls_close_w(rec);
    return 0;
}
static int64_t g_port, g_cfg;
static void *srv(void *a) { (void)a; tk_tls_listen_w(g_port, g_cfg, I(on_conn)); return NULL; }

int main(void)
{
    char a0[] = "seal", a1[] = "--allow-all";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */

    int64_t kp = tk_tls_genselfsigned_w(I("localhost"), 1);
    if (!kp) { printf("  FAIL: keygen\n"); return 1; }
    const TkTlsKeypairRec *k = (const TkTlsKeypairRec *)(intptr_t)kp;
    const char *cert = P(tk_tls_certof_w(kp));
    CHECK(sealed_eq(cert, k->certpem) && cert != k->certpem
          && P(tk_tls_keyof_w(kp)) == k->keypem && tks_hdrlen(k->keypem) < 0,
          "tls.certof: a sealed copy of the PEM; tls.keyof: the keypair's own buffer");

    char *fp_raw = tls_fingerprint(k->certpem);
    const char *fp = P(tk_tls_fingerprint_w(I(cert)));
    CHECK(sealed_eq(fp, fp_raw) && strlen(fp) == 64
          && sealed_eq(P(tk_tls_fingerprint_w(I(""))), ""),
          "tls.fingerprint: sealed, 64 hex chars, as for the unsealed PEM; of \"\" a sealed \"\"");
    free(fp_raw);

    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    bind(probe, (struct sockaddr *)&a, sizeof a); getsockname(probe, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port); close(probe);
    g_cfg = tk_tls_tlsconfig_w(I(cert), tk_tls_keyof_w(kp));
    pthread_t t; pthread_create(&t, NULL, srv, NULL); pthread_detach(t);

    int64_t ccfg = tk_tls_pinconfig_w(I(""), I(""), I(cert));
    int64_t c = 0;
    for (int i = 0; i < 100 && !c; i++) {
        c = tk_tls_connect_w(I("127.0.0.1"), g_port, ccfg);
        if (!c) usleep(20000);
    }
    if (!c) { printf("  FAIL: tls.connect never succeeded\n"); return 1; }
    CHECK(sealed_nonempty(conn_id(c)), "tls.connect: the $TlsConn .id is sealed");
    CHECK(tk_tls_write_w(c, I(tks_strdup("ping"))) == 1
          && sealed_eq(P(tk_tls_read_w(c)), "pong"),
          "tls.write of a sealed str delivers it; tls.read is sealed (both directions)");
    const char *pc = P(tk_tls_peercert_w(c));
    CHECK(sealed_nonempty(pc) && strncmp(pc, "-----BEGIN CERTIFICATE-----", 27) == 0
          && strcmp(P(tk_tls_fingerprint_w(I(pc))), fp) == 0,
          "tls.peercert: sealed, the server's certificate");
    CHECK(sealed_eq(P(tk_tls_protocol_w(c)), "TLSv1.3")
          && sealed_nonempty(P(tk_tls_group_w(c)))
          && sealed_eq(P(tk_tls_pairingcode_w(c)), "000000"),
          "client tls.protocol, tls.group and tls.pairingcode sealed");
    for (int i = 0; i < 300 && !srv_done; i++) usleep(10000);
    CHECK(srv_done && srv_ok,
          "server handler: .id, tls.read, tls.protocol, tls.group, tls.pairingcode sealed");
    tk_tls_close_w(c);
    CHECK(sealed_eq(P(tk_tls_group_w(0)), "") && sealed_eq(P(tk_tls_protocol_w(0)), ""),
          "tls.group / tls.protocol of no connection: a sealed \"\"");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE -DTK_HAVE_OPENSSL -g -O1 ${SAN} -Wno-deprecated-declarations -iquote "${S}" -I"${S}" ${OSSL_CFLAGS})
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects tls -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
# shellcheck disable=SC2086
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" ${OSSL_LIBS} -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=tls:std.tls;
f=main():i64{
  let c=mt tls.genselfsigned("localhost";1){$ok:kp tls.certof(kp);$err:e ""};
  let fp=tls.fingerprint(c);
  if(c.len>100){io.println("cert ok fp=\(fp.len)")}el{io.println("keygen failed")};
  let g=tls.group(0);
  io.println("g=[\(g)] \(g.len)");
  <0
};
TOKE
WANT='cert ok fp=64
g=[] 0'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: tls.certof / fingerprint / group and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "---------------------------------------------------------------------"
echo "C1660: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
