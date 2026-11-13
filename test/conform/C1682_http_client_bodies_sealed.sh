#!/usr/bin/env bash
# C1682_http_client_bodies_sealed.sh -- the response bodies the simple HTTP
# client calls hand toke are sealed strings (story 127.295, S1 step 5 final
# tranche; runtime-abi §3.1).
#
# http.get(url), http.posturl(url; body), http.post(c; path; body),
# http.put(c; path; body) and http.delete(c; path) in tk_web_glue.c copied
# the response body into a malloc'd C string.  They now make a sealed copy
# (body_seal), measured as a C string -- a body holding a 0 byte reads as the
# bytes before it, exactly as the malloc'd copy did -- and free the
# client's buffer as before.
#
# WHAT IS ASSERTED (C driver, ASan+UBSan, TK_STR_VERIFY=1, a one-shot
# loopback server thread per request):
#   1. http.get: the body is sealed and right
#   2. http.posturl: sealed, and the server saw the (sealed) request body
#   3. http.post / http.put / http.delete through a client: sealed
#   4. a body holding a 0 byte: sealed with the C-string length
#
# Negative control: C1682_STDLIB=<dir> uses another tree's src/stdlib.
#   Base c3051e18's src/stdlib: 0 passed, 4 failed -- every body "not sealed".
# Measured: this branch 4 passed.
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1682_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1682_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1682: http client response bodies sealed (127.295)"
echo "---------------------------------------------------"

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
#include "capabilities.h"
int64_t tk_http_get_w(int64_t url);
int64_t tk_http_posturl_w(int64_t url, int64_t body);
int64_t tk_http_client_w(int64_t base);
int64_t tk_http_post_w(int64_t c, int64_t path, int64_t body);
int64_t tk_http_put_w(int64_t c, int64_t path, int64_t body);
int64_t tk_http_delete_w(int64_t c, int64_t path);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int lfd, port;
static char seen[4096];
static const char *reply_body; static size_t reply_len;
/* One request per call: read it whole (headers + Content-Length), answer. */
static void *serve(void *arg)
{
    int n = (int)(intptr_t)arg;
    for (int k = 0; k < n; k++) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) return NULL;
        size_t got = 0; long want = -1;
        while (got < sizeof seen - 1) {
            ssize_t r = read(c, seen + got, sizeof seen - 1 - got);
            if (r <= 0) break;
            got += (size_t)r; seen[got] = 0;
            char *e = strstr(seen, "\r\n\r\n");
            if (e) {
                char *cl = strstr(seen, "Content-Length: ");
                if (!cl) cl = strstr(seen, "content-length: ");
                want = cl ? atol(cl + 16) : 0;
                if ((long)(got - (size_t)(e + 4 - seen)) >= want) break;
            }
        }
        char hdr[256];
        int hl = snprintf(hdr, sizeof hdr, "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                          "Connection: close\r\n\r\n", reply_len);
        (void)!write(c, hdr, (size_t)hl);
        (void)!write(c, reply_body, reply_len);
        close(c);
    }
    return NULL;
}
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.40s')\n", s); return 0; }
    if (strcmp(s, want) != 0) printf("        (got '%.40s')\n", s);
    return (size_t)h == strlen(s) && strcmp(s, want) == 0;
}
static void reply(const char *b, size_t n) { reply_body = b; reply_len = n; }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    int one = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 8)) { printf("  FAIL: bind\n"); return 1; }
    socklen_t al = sizeof a; getsockname(lfd, (struct sockaddr *)&a, &al); port = ntohs(a.sin_port);
    pthread_t th; pthread_create(&th, NULL, serve, (void *)(intptr_t)6);
    char url[128], base[64];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/x", port);
    snprintf(base, sizeof base, "http://127.0.0.1:%d", port);

    reply("hello-get", 9);
    CHECK(sealed_eq(P(tk_http_get_w(I(url))), "hello-get"), "http.get: the body is sealed");
    reply("posted", 6);
    const char *pb = tks_dup("{\"k\":1}", 7);
    CHECK(sealed_eq(P(tk_http_posturl_w(I(url), I(pb))), "posted") && strstr(seen, "{\"k\":1}"),
          "http.posturl: sealed; the server saw the sealed request body");
    int64_t c = tk_http_client_w(I(base));
    reply("via-client", 10);
    int ok = sealed_eq(P(tk_http_post_w(c, I("/p"), I("b"))), "via-client");
    ok &= sealed_eq(P(tk_http_put_w(c, I("/p"), I("b"))), "via-client");
    ok &= sealed_eq(P(tk_http_delete_w(c, I("/p"))), "via-client");
    CHECK(c && ok, "http.post / http.put / http.delete: sealed");
    reply("ab\0cd", 5);
    const char *z = P(tk_http_get_w(I(url)));
    CHECK(sealed_eq(z, "ab") && tks_hdrlen(z) == 2, "a body holding a 0: sealed with the C-string length");
    pthread_join(th, NULL);
    return bad ? 1 : 0;
}
C
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects http -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 perl -e 'alarm 60; exec @ARGV' ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && grep -a -q -E 'ERROR: AddressSanitizer|runtime error' seal.out; then
        fail "C harness stopped: $(grep -a -m1 -o -E 'AddressSanitizer: [a-z-]+|runtime error: .{0,60}' seal.out)"
    elif [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

echo "---------------------------------------------------"
echo "C1682: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
