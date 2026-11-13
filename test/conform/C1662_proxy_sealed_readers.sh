#!/usr/bin/env bash
# C1662_proxy_sealed_readers.sh -- proxy.c reads every length with tk_strlen
# or from the TkStrBuf that built the string (story 127.295, S1 step 5
# tranche 15; runtime-abi §3.1).
#
# proxy.c hands toke no string (no toke surface reaches proxy_forward yet;
# C464), so nothing there is sealed.  Its three readers were the request
# body's length in proxy_build_request, the built request's length in
# proxy_forward, and the health probe's request length.  A readers-only
# conversion must not change a byte on the wire, so:
#
# WHAT IS ASSERTED (ASan+UBSan, under TK_STR_VERIFY=1, so every trusted
# length read asserts header == strlen; the harness #includes proxy.c to
# reach the static proxy_build_request):
#   1. proxy_build_request with a sealed body, method, path and header
#      values builds exactly the bytes the same request with unsealed copies
#      builds, Content-Length included;
#   2. proxy_health_probe with a sealed path sends one complete request (it
#      ends in the blank line, carries the path) to a loopback backend and
#      reads its 200 as healthy;
#   3. proxy_forward of a request with a sealed body delivers the body and
#      a Content-Length equal to its length to a loopback backend.
#
# Negative control: C1662_STDLIB=<dir> uses another tree's src/stdlib.  On
# base 75ec5f49 every case passes too (an unsealed reader was always
# correct): this test guards the conversion, it does not detect its absence
# -- the ratchet (scripts/check_str_strlen.py) does that.
#
# Measured: this branch 3 passed; base 3 passed.
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1662_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1662_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1662: proxy.c reads sealed strings with tk_strlen (127.295)"
echo "---------------------------------------------------------------------"

cat > seal.c <<'C'
#include "proxy.c"
#include "tk_strhdr.h"
#include <pthread.h>
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)

/* A one-shot loopback backend: reads one request, answers 200. */
static int g_lfd, g_port;
static char g_got[8192];
static void *backend(void *a)
{
    (void)a;
    int c = accept(g_lfd, NULL, NULL);
    if (c < 0) return NULL;
    size_t n = 0;
    struct timeval tv = { 2, 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (;;) {
        ssize_t r = read(c, g_got + n, sizeof g_got - 1 - n);
        if (r <= 0) break;
        n += (size_t)r; g_got[n] = '\0';
        char *e = strstr(g_got, "\r\n\r\n");
        if (e) {
            const char *cl = strcasestr(g_got, "\r\nContent-Length: ");
            size_t want = cl ? (size_t)atoi(cl + 18) : 0;
            if (n >= (size_t)(e + 4 - g_got) + want) break;
        }
    }
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    (void)!write(c, resp, strlen(resp));
    close(c);
    return NULL;
}
static void start_backend(void)
{
    g_got[0] = '\0';
    g_lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(g_lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    bind(g_lfd, (struct sockaddr *)&a, sizeof a); listen(g_lfd, 1);
    getsockname(g_lfd, (struct sockaddr *)&a, &al);
    g_port = ntohs(a.sin_port);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    const char *body = "hello sealed body";
    char *sbody = tks_strdup(body), *spath = tks_strdup("/p?q=1");
    char *sval = tks_strdup("text/plain"), *smeth = tks_strdup("POST");
    int sealed = tks_hdrlen(sbody) >= 0 && tks_hdrlen(spath) >= 0
                 && tks_hdrlen(sval) >= 0 && tks_hdrlen(smeth) >= 0;

    /* 1. proxy_build_request: sealed and unsealed requests, the same bytes. */
    StrPair hs[] = { { "Accept", NULL } };
    Req rs; memset(&rs, 0, sizeof rs);
    rs.method = smeth; rs.path = spath; rs.body = sbody;
    hs[0].val = sval; rs.headers.data = hs; rs.headers.len = 1;
    StrPair hu[] = { { "Accept", "text/plain" } };
    Req ru = rs; ru.method = "POST"; ru.path = "/p?q=1"; ru.body = (char *)body;
    ru.headers.data = hu;
    char *os = proxy_build_request(&rs, "backend", 8080, "192.0.2.7");
    char *ou = proxy_build_request(&ru, "backend", 8080, "192.0.2.7");
    CHECK(sealed && os && ou && strcmp(os, ou) == 0
          && strstr(os, "\r\nContent-Length: 17\r\n") && strstr(os, "\r\n\r\nhello sealed body"),
          "proxy_build_request: a sealed request builds the unsealed one's bytes");
    free(os); free(ou);

    /* 2. proxy_health_probe with a sealed path. */
    start_backend();
    pthread_t t; pthread_create(&t, NULL, backend, NULL);
    ProxyBackend b; memset(&b, 0, sizeof b);
    b.host = (char *)"127.0.0.1"; b.port = g_port;
    int rc = proxy_health_probe(&b, spath, 2);
    pthread_join(t, NULL); close(g_lfd);
    size_t gl = strlen(g_got);
    CHECK(rc == 0 && strncmp(g_got, "GET /p?q=1 HTTP/1.1\r\n", 21) == 0
          && gl >= 4 && strcmp(g_got + gl - 4, "\r\n\r\n") == 0,
          "proxy_health_probe: a sealed path sends one complete request, 200 is healthy");

    /* 3. proxy_forward of a sealed body. */
    start_backend();
    pthread_create(&t, NULL, backend, NULL);
    ProxyUpstream *up = proxy_upstream_new("c1662", LB_ROUND_ROBIN);
    proxy_upstream_add(up, "127.0.0.1", g_port, 1);
    Res res = proxy_forward(up, rs, "192.0.2.7");
    pthread_join(t, NULL); close(g_lfd);
    CHECK(res.status == 200 && strstr(g_got, "\r\nContent-Length: 17\r\n")
          && strstr(g_got, "\r\n\r\nhello sealed body"),
          "proxy_forward: a sealed body reaches the backend with its Content-Length");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.  seal.c #includes proxy.c itself.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -Wno-unused-function -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --except proxy.c \
        --cc "${CC}" --objects http -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

echo "---------------------------------------------------------------------"
echo "C1662: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
