#!/usr/bin/env bash
# C590_tls_acme_hardening.sh -- std.tls connection registry and the ACME
# client's storage, payload and renewal paths (story 121.19; audit-120
# tls-acme.md).  Every case is a C harness built and run under ASan+UBSan.
#
# WHAT IS ASSERTED, and what main (f605aec9) did
#   tls  TLS-03  a tls_read waiting for data does not stop tls_write on
#                ANOTHER connection -- main: the write blocked on the global
#                registry mutex the reader held across SSL_read (watchdog)
#        TLS-03  tls_close of a connection with a blocked reader returns, and
#                the reader wakes -- main: tls_close blocked on the same mutex
#        TLS-03  200 rounds of tls_close racing tls_write/tls_protocol on the
#                same id: no ASan report (main: close freed the SSL* after
#                dropping the lock and before removing the entry)
#        TLS-06  a connection id carries no pointer -- main: "%d:%p"
#   acme ACME-02 the HTTP-01 handler serving while the table is cleared and
#                refilled: no ASan report (main: unlocked globals, freed
#                key_auth returned as the body)
#        ACME-03 a domain carrying '"' or ',' is refused; an email with '"' is
#                JSON-escaped -- main: both interpolated raw
#        ACME-04 an EXPIRED certificate triggers renewal -- main: days < 0 was
#                read as "error", so a lapsed certificate was never renewed
#        ACME-05 a symlink planted at "<path>.tmp.<pid>" is not followed --
#                main: O_CREAT|O_TRUNC wrote through it into the target
#        ACME-07 an account key readable by group/others is refused
#
# Negative control: C590_STDLIB=<dir> points the suite at another stdlib tree
# (a copy of src/stdlib).  With main's tls.c the tls case fails on its
# watchdog; with each acme fix reverted in a copy, the matching acme check
# prints BAD.  Recorded in the 121.19 report.
#
# acme and tls need OpenSSL headers; without them this suite is a named
# CONFORM-SKIP.
#
# Story: 121.19
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C590_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c590_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C590: std.tls registry + ACME client hardening (121.19)"
echo "------------------------------------------------------------------------"

OSSL_CFLAGS=""; OSSL_LIBS=""
if pkg-config --exists openssl 2>/dev/null; then
    OSSL_CFLAGS="$(pkg-config --cflags openssl)"; OSSL_LIBS="$(pkg-config --libs openssl)"
else
    for pfx in /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3; do
        if [ -f "${pfx}/include/openssl/ssl.h" ]; then
            OSSL_CFLAGS="-I${pfx}/include"; OSSL_LIBS="-L${pfx}/lib -lssl -lcrypto"; break
        fi
    done
    if [ -z "${OSSL_LIBS}" ] && [ -f /usr/include/openssl/ssl.h ]; then OSSL_LIBS="-lssl -lcrypto"; fi
fi
if [ -z "${OSSL_LIBS}" ]; then
    echo "CONFORM-SKIP: no OpenSSL headers on this host (std.tls and acme need them)"
    exit 0
fi

# 127.268: the stdlib link sets come from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not hand lists that a stdlib
# dependency change silently breaks.  t_acme.c #includes acme.c itself, so
# it is left out of the acme set.
# 127.270: the link sets are cached objects built with case_run's flags.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -DTK_HAVE_OPENSSL ${OSSL_CFLAGS}
    -Wno-deprecated-declarations -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" --objects tls -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK_TLS=(); while IFS= read -r l; do LINK_TLS+=("$l"); done <<< "${LINKSET}"
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except acme.c --cc "${CC}" \
        --objects http -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK_ACME=(); while IFS= read -r l; do LINK_ACME+=("$l"); done <<< "${LINKSET}"

# case NAME SOURCE-FILE LINK-ARGS... -- build + run; the program prints
# "OK <what>" / "BAD <what>" lines.
case_run() {
    local name="$1" src="$2"; shift 2
    if ! "${CC}" "${CF[@]}" \
            -o "${WORK}/${name}" "${src}" "$@" >"${WORK}/${name}.cc" 2>&1; then
        fail "${name}: build: $(grep -a -m3 -E 'error' "${WORK}/${name}.cc" | head -c 400)"
        return
    fi
    (cd "${WORK}" && "./${name}") >"${WORK}/${name}.out" 2>&1
    local rc=$?
    local ok bad
    ok=$(grep -a -c '^OK ' "${WORK}/${name}.out")
    bad=$(grep -a -c '^BAD ' "${WORK}/${name}.out")
    grep -a -E '^(OK|BAD) ' "${WORK}/${name}.out" | sed "s/^OK /  ok   ${name}: /; s/^BAD /  bad  ${name}: /"
    if [ "${rc}" -ne 0 ] && [ "${bad}" -eq 0 ]; then
        fail "${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/${name}.out" | head -c 300)"
    elif [ "${bad}" -ne 0 ] || [ "${ok}" -eq 0 ]; then
        fail "${name}: ${bad} check(s) failed"
    else
        pass "${name}: ${ok} check(s)"
    fi
}

# ── tls: registry (TLS-03, TLS-06) ─────────────────────────────────────────
cat > "${WORK}/t_tls.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "tls.h"
#include "capabilities.h"
#define CHECK(c, m) do { printf("%s %s\n", (c) ? "OK" : "BAD", m); fflush(stdout); } while (0)

static const char *g_watch = "startup";
static void on_alarm(int sig) {
    (void)sig;
    char b[256];
    int n = snprintf(b, sizeof b, "BAD watchdog: stuck in \"%s\"\n", g_watch);
    write(1, b, (size_t)n);
    _exit(1);
}
static void watch(const char *what, unsigned secs) { g_watch = what; alarm(secs); }

static void on_conn(TlsConn c) { (void)c; /* hold it open; never read */ }
static int g_port;
static TlsConfig g_srv;
static void *server(void *a) { (void)a; tls_listen(g_port, g_srv, on_conn); return NULL; }

static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&sa, sizeof sa);
    socklen_t l = sizeof sa; getsockname(fd, (struct sockaddr *)&sa, &l);
    close(fd);
    return ntohs(sa.sin_port);
}

static TlsConn connect_one(void) {
    /* 121.64: an unpinned client now verifies the server; pin the test
     * server's self-signed certificate (portable to a pre-121.64 tls.c). */
    TlsConfig cc = {NULL, NULL, g_srv.cert_pem, 0};
    for (int i = 0; i < 100; i++) {
        TlsConnResult r = tls_connect("127.0.0.1", g_port, cc);
        if (!r.is_none) return r.conn;
        usleep(20000);
    }
    TlsConn none = {NULL};
    return none;
}

static TlsConn g_reader_conn;
static volatile int g_read_done = 0;
static void *reader(void *a) { (void)a; TlsStrResult r = tls_read(g_reader_conn); free(r.ok); g_read_done = 1; return NULL; }

static char *g_race_id;
static void *racer(void *a) {
    (void)a;
    TlsConn c = { g_race_id };
    for (int i = 0; i < 2000; i++) {
        if (!tls_write(c, "x")) break;
        free(tls_protocol(c));
    }
    return NULL;
}

int main(void) {
    char *av[] = { "t", "--allow-net", NULL };
    tk_cap_init(2, av);
    signal(SIGALRM, on_alarm);
    signal(SIGPIPE, SIG_IGN);
    TlsKeypairResult kp = tls_gen_self_signed("c590.test", 1);
    if (kp.is_err) { printf("BAD keypair: %s\n", kp.err_msg); return 1; }
    g_srv.cert_pem = kp.cert_pem; g_srv.key_pem = kp.key_pem;
    g_port = free_port();
    pthread_t st; pthread_create(&st, NULL, server, NULL); pthread_detach(st);

    watch("connect", 10);
    TlsConn c1 = connect_one(), c2 = connect_one();
    alarm(0);
    CHECK(c1.id && c2.id, "two connections to a local tls.listen");
    if (!c1.id || !c2.id) return 1;

    /* TLS-06 */
    int digits_colon_only = 1;
    for (const char *p = c1.id; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == ':')) digits_colon_only = 0;
    CHECK(digits_colon_only && !strstr(c1.id, "0x"),
          "TLS-06: connection id is slot:generation, no pointer");

    /* TLS-03 (a): c1 blocked in tls_read must not block c2 */
    g_reader_conn = c1;
    pthread_t rt; pthread_create(&rt, NULL, reader, NULL);
    usleep(300000);
    watch("tls_write on another connection while one reader blocks", 5);
    int w = tls_write(c2, "hello");
    alarm(0);
    CHECK(w == 1, "TLS-03: tls_write(c2) completes while tls_read(c1) waits");

    /* TLS-03 (b): closing c1 wakes its reader */
    watch("tls_close of a connection with a blocked reader", 5);
    int cl = tls_close(c1);
    for (int i = 0; i < 300 && !g_read_done; i++) usleep(10000);
    alarm(0);
    CHECK(cl == 1, "TLS-03: tls_close returns with a reader blocked");
    CHECK(g_read_done, "TLS-03: the blocked reader wakes and returns");
    watch("join reader", 5); pthread_join(rt, NULL); alarm(0);
    TlsStrResult after = tls_read(c1);
    CHECK(after.is_none && tls_write(c1, "x") == 0 && tls_close(c1) == 0,
          "closed id: read none, write 0, second close 0");

    /* TLS-03 (c): close races use of the same id -- ASan is the judge */
    watch("close/use race", 60);
    int rounds = 0;
    for (int i = 0; i < 200; i++) {
        TlsConn c = connect_one();
        if (!c.id) break;
        g_race_id = c.id;
        pthread_t t; pthread_create(&t, NULL, racer, NULL);
        if (i & 1) usleep(200);
        tls_close(c);
        pthread_join(t, NULL);
        free(c.id);
        rounds++;
    }
    alarm(0);
    CHECK(rounds == 200, "TLS-03: 200 close-vs-use rounds, no sanitizer report");
    tls_close(c2);
    return 0;
}
C
# shellcheck disable=SC2086
case_run tls "${WORK}/t_tls.c" "${LINK_TLS[@]}" ${OSSL_LIBS} -lpthread -lm

# ── acme ────────────────────────────────────────────────────────────────────
cat > "${WORK}/t_acme.c" <<C
#include "${S}/acme.c"
C
cat >> "${WORK}/t_acme.c" <<'C'
#include "capabilities.h"
#include <sys/stat.h>
#define CHECK(c, m) do { printf("%s %s\n", (c) ? "OK" : "BAD", m); fflush(stdout); } while (0)

static char *slurp(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    static char b[8192]; size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return b;
}

static int write_cert(const char *path, long not_after_secs) {
    EVP_PKEY *k = acme_gen_key();
    X509 *x = X509_new();
    X509_set_version(x, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
    X509_gmtime_adj(X509_get_notBefore(x), -90L * 86400L);
    X509_gmtime_adj(X509_get_notAfter(x), not_after_secs);
    X509_NAME *n = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC, (const unsigned char *)"c590", -1, -1, 0);
    X509_set_issuer_name(x, n);
    X509_set_pubkey(x, k);
    X509_sign(x, k, EVP_sha256());
    FILE *f = fopen(path, "w"); if (!f) return -1;
    PEM_write_X509(f, x); fclose(f);
    X509_free(x); EVP_PKEY_free(k);
    return 0;
}

static volatile int g_stop = 0;
static void *hammer(void *a) {
    (void)a;
    Req rq; memset(&rq, 0, sizeof rq);
    rq.path = "/.well-known/acme-challenge/tok0";
    long served = 0;
    while (!g_stop) {
        Res r = acme_challenge_handler(rq);
        if (r.status == 200 && r.body) served += (long)strlen(r.body);
    }
    return (void *)served;
}

int main(void) {
    char *av[] = { "t", "--allow-net", "--allow-read", "--allow-write", NULL };
    tk_cap_init(4, av);

    /* ACME-03 */
    const char *bad1[] = { "a.example\"},{\"type\":\"dns\",\"value\":\"evil.example" };
    const char *bad2[] = { "a.example,DNS:evil.example" };
    const char *good[] = { "a.example", "*.b.example" };
    char *j1 = acme_identifiers_json(bad1, 1), *j2 = acme_san_list(bad2, 1);
    char *j3 = acme_identifiers_json(good, 2);
    CHECK(j1 == NULL, "ACME-03: a domain carrying '\"' is refused (newOrder JSON)");
    CHECK(j2 == NULL, "ACME-03: a domain carrying ',' is refused (SAN list)");
    CHECK(j3 && strstr(j3, "\"value\":\"*.b.example\""), "ACME-03: ordinary + wildcard domains accepted");
    free(j1); free(j2); free(j3);
    char *pl = acme_account_payload("x@y.example\"],\"onlyReturnExisting\":true,\"z\":[\"");
    CHECK(pl && strstr(pl, "x@y.example\\\"],\\\"onlyReturnExisting") && !strstr(pl, "\"onlyReturnExisting\":true"),
          "ACME-03: '\"' in the email is escaped, no field injected");
    free(pl);

    /* ACME-04 */
    AcmeClient ac; memset(&ac, 0, sizeof ac);   /* no account_url: an order fails at once */
    const char *dom[] = { "a.example" };
    write_cert("expired.pem", -3L * 86400L);
    write_cert("fresh.pem", 80L * 86400L);
    write_cert("soon.pem", 10L * 86400L);
    CHECK(acme_check_renewal(&ac, NULL, "expired.pem", "k.pem", dom, 1) == -1,
          "ACME-04: an EXPIRED certificate is renewed (attempted; fails here with no account)");
    CHECK(acme_check_renewal(&ac, NULL, "soon.pem", "k.pem", dom, 1) == -1,
          "ACME-04: 10 days left is renewed (attempted)");
    CHECK(acme_check_renewal(&ac, NULL, "fresh.pem", "k.pem", dom, 1) == 0,
          "ACME-04: 80 days left is not renewed");
    CHECK(acme_check_renewal(&ac, NULL, "missing.pem", "k.pem", dom, 1) == 0,
          "ACME-04: an unreadable certificate is not an expiry");

    /* ACME-05 */
    FILE *v = fopen("victim.txt", "w"); fputs("VICTIM", v); fclose(v);
    char tmpname[256]; snprintf(tmpname, sizeof tmpname, "out.pem.tmp.%d", (int)getpid());
    symlink("victim.txt", tmpname);
    int wr = acme_write_file("out.pem", (const uint8_t *)"CERTDATA", 8);
    char *vs = slurp("victim.txt");
    CHECK(vs && strcmp(vs, "VICTIM") == 0, "ACME-05: planted temp symlink not followed (victim intact)");
    char *os = slurp("out.pem");
    struct stat st; int sr = lstat("out.pem", &st);
    CHECK(wr == 0 && os && strcmp(os, "CERTDATA") == 0 && sr == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600,
          "ACME-05: target written, regular file, mode 0600");
    /* the key writer goes through the same path */
    symlink("victim.txt", "cert.key");
    EVP_PKEY *k = acme_gen_key();
    int kw = acme_write_key("cert.key", k);
    vs = slurp("victim.txt");
    sr = lstat("cert.key", &st);
    CHECK(kw == 0 && vs && strcmp(vs, "VICTIM") == 0 && sr == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600,
          "ACME-05: key write replaces a symlink at the key path, does not write through it");

    /* ACME-07 */
    AcmeClient a2; memset(&a2, 0, sizeof a2);
    chmod("cert.key", 0644);
    CHECK(acme_load_account_key(&a2, "cert.key") == -1 && a2.account_key == NULL,
          "ACME-07: a group/other-readable account key is refused");
    chmod("cert.key", 0600);
    CHECK(acme_load_account_key(&a2, "cert.key") == 0 && a2.account_key != NULL,
          "ACME-07: a 0600 account key loads");
    EVP_PKEY_free(a2.account_key); a2.account_key = NULL;
    symlink("cert.key", "link.key");
    CHECK(acme_load_account_key(&a2, "link.key") == -1, "ACME-07: an account key is not read through a symlink");
    EVP_PKEY_free(k);

    /* ACME-02: serve while the table churns -- ASan is the judge */
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, hammer, NULL);
    for (int i = 0; i < 20000; i++) {
        acme_challenge_add("tok0", "tok0.THUMBPRINT-THUMBPRINT-THUMBPRINT-THUMBP");
        acme_challenges_clear();
    }
    g_stop = 1;
    long total = 0;
    for (int i = 0; i < 4; i++) { void *r; pthread_join(t[i], &r); total += (long)r; }
    CHECK(1, "ACME-02: 20000 add/clear rounds under 4 serving threads, no sanitizer report");
    (void)total;
    return 0;
}
C
# shellcheck disable=SC2086
case_run acme "${WORK}/t_acme.c" "${LINK_ACME[@]}" ${OSSL_LIBS} -lz -lpthread -lm

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
