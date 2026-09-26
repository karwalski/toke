#!/usr/bin/env bash
# C113_route_param_bound.sh — route matching is bounded (story 127.146).
#
# THE DEFECT.  http.c's match_pattern wrote each `:param` binding into the
# caller's StrPair[32] with no bound; a route with 33+ params wrote past the
# stack array.  127.72 unified the two copies and added a bound, but the
# bound silently DROPPED the extra bindings and still reported a match, so a
# handler read "" for them.  router.c's split_path had the sibling defect:
# it stopped at 64 segments and discarded the rest, so a 65-segment request
# matched a 64-segment route.
#
# WHAT THIS DRIVES (built with -fsanitize=address,undefined):
#   http_match_pattern: 32 params -> match, 32 exact bindings;
#                       33 params -> no match, 0 bindings, nothing leaked;
#                       100 params -> no match.
#   router_dispatch:    a 32-param route runs its handler and sees p31;
#                       a 33-param route -> 404, handler not called;
#                       a 64-literal route does not answer a 65-segment path.
# ASan checks every write.  Leaks are not checked (detect_leaks=0: macOS
# has no LeakSanitizer); set C113_LEAKS=1 on Linux to check them too.
#
# Negative control: put back `if (*cnt < HTTP_MAX_PARAMS) {...} else
# {free(key); free(val);}` in http_match_pattern -> "33 params: want
# no-match"; put back `while (*p && n < max_segs)` in split_path ->
# "65-segment path must not match a 64-segment route".  Removing the bound
# altogether -> ASan stack-buffer-overflow.
#
# Story: 127.146

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c113_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C113: route :param matching is bounded"
echo "--------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "router.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* pattern "/:p0/:p1/..." and path "/v0/v1/..." with n segments */
static void build(int n, char *pat, char *path, int lit) {
    pat[0] = path[0] = '\0';
    for (int i = 0; i < n; i++) {
        char a[32], b[32];
        if (lit) { snprintf(a, sizeof a, "/s%d", i); snprintf(b, sizeof b, "/s%d", i); }
        else     { snprintf(a, sizeof a, "/:p%d", i); snprintf(b, sizeof b, "/v%d", i); }
        strcat(pat, a); strcat(path, b);
    }
}

static int called = 0;
static char seen[64];
static TkRouteResp h(TkRouteCtx ctx) {
    called++;
    seen[0] = '\0';
    for (uint64_t i = 0; i < ctx.nparam; i++)
        if (!strcmp(ctx.param_names[i], "p31")) snprintf(seen, sizeof seen, "%s", ctx.param_values[i]);
    return router_resp_ok("ok", "text/plain");
}

int main(void) {
    static char pat[4096], path[4096];
    StrPair out[HTTP_MAX_PARAMS]; int cnt = -1;

    build(32, pat, path, 0);
    int m = http_match_pattern(pat, path, out, &cnt);
    int good = m == 1 && cnt == 32 && !strcmp(out[31].key, "p31") && !strcmp(out[31].val, "v31");
    CHECK(good, "32 params: match with 32 exact bindings");
    if (m) http_free_params(out, &cnt);

    build(33, pat, path, 0);
    cnt = -1;
    m = http_match_pattern(pat, path, out, &cnt);
    CHECK(m == 0 && cnt == 0, "33 params: want no-match and 0 bindings left");
    if (m) http_free_params(out, &cnt);

    build(100, pat, path, 0);
    cnt = -1;
    m = http_match_pattern(pat, path, out, &cnt);
    CHECK(m == 0 && cnt == 0, "100 params: no-match");
    if (m) http_free_params(out, &cnt);

    TkRouter *r = router_new();
    static char p32[4096], q32[4096], p33[4096], q33[4096], p64[4096], q64[4096];
    build(32, p32, q32, 0);
    build(33, p33, q33, 0);
    build(64, p64, q64, 1);
    /* distinct first segments keep the three routes apart */
    char r32[4200], r33[4200], r64[4200], u32[4200], u33[4200], u65[4200];
    snprintf(r32, sizeof r32, "/a%s", p32); snprintf(u32, sizeof u32, "/a%s", q32);
    snprintf(r33, sizeof r33, "/b%s", p33); snprintf(u33, sizeof u33, "/b%s", q33);
    snprintf(r64, sizeof r64, "%s", p64);   snprintf(u65, sizeof u65, "%s/extra", q64);
    router_get(r, r32, h); router_get(r, r33, h); router_get(r, r64, h);

    called = 0;
    TkRouteResp a = router_dispatch(r, "GET", u32, NULL, NULL);
    CHECK(a.status == 200 && called == 1 && !strcmp(seen, "v31"), "router: 32-param route runs, sees p31=v31");
    called = 0;
    TkRouteResp b = router_dispatch(r, "GET", u33, NULL, NULL);
    CHECK(b.status == 404 && called == 0, "router: 33-param route is a 404");
    called = 0;
    TkRouteResp c = router_dispatch(r, "GET", u65, NULL, NULL);
    CHECK(c.status == 404 && called == 0, "router: 65-segment path must not match a 64-segment route");
    called = 0;
    TkRouteResp d = router_dispatch(r, "GET", q64, NULL, NULL);
    CHECK(d.status == 200 && called == 1, "router: the 64-segment route still answers its own path");
    router_free(r);

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" \
        "${S}/http.c" "${S}/encoding.c" "${S}/str.c" "${S}/log.c" "${S}/capabilities.c" \
        "${S}/router.c" "${S}/ws.c" -lz -lpthread >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 "${WORK}/cc.log")"
    exit 1
fi
echo "  PASS: built with ${SAN}"
ASAN_OPTIONS="detect_leaks=${C113_LEAKS:-0}:abort_on_error=0" "${WORK}/t"
