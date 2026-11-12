#!/usr/bin/env bash
# C1310_router_c_api_tables_grow.sh -- std.router's C registration API keeps
# everything it is given or says it did not: router_static, router_ws and
# router_use have no count cap, and every registration function returns
# 0 / -1 with a DIAGNOSTIC line on failure (story 127.349).
#
# THE DEFECT (fix/z142-router-cap 75ad77ba).  router.c's C-only API returned
# void and dropped registrations silently:
#   - router_static kept its prefixes in a process-wide StaticEntry[16] (one
#     macro trampoline per slot, never released): the 17th router_static in
#     the process -- on any router -- returned without registering, and its
#     prefix answered 404;
#   - router_ws returned past MAX_WS_ROUTES=16, router_use past
#     MAX_MIDDLEWARE=64 (so router_use_cors/etag/gzip did nothing);
#   - add_route returned silently when malloc failed, and did not check
#     strdup: a node with a NULL method or pattern was linked in and the
#     matcher dereferenced it on the next dispatch;
#   - a router_static prefix past 511 bytes / directory past 1023 bytes and
#     a router_ws pattern past 511 bytes were truncated.
#
# THE FIX.  The middleware chain and the WebSocket routes are growing
# arrays; a router_static route carries its own StaticEntry (TkRoute.udata,
# freed with the route) and one shared handler finds it through the route
# the dispatch matched.  router_get/post/put/delete, router_use,
# router_use_cors/etag/gzip, router_static and router_ws return int: 0 kept,
# -1 not kept (NULL router/pattern/handler, out of memory), and a -1 writes
# DIAGNOSTIC: kind=router_register_failed to stderr.  add_route links
# nothing unless every allocation succeeded.  router_glue.c's register_route
# takes back its handler entry when the C router refuses the route.
#
# WHAT IS ASSERTED.
#   1. C, ASan+UBSan:
#        - 1000 router_static prefixes /p0/../p999/, each its own directory:
#          every registration returns 0 and every prefix serves its own file;
#          a second router's prefix, registered after, serves too; a 600-byte
#          prefix serves; an unregistered prefix and a
#          missing file under a registered one are 404;
#        - 1000 router_use middleware: all 1000 run on one request, and the
#          1001st (router_use_etag) adds its ETag;
#        - 1000 router_ws endpoints /ws/0../ws/999: each upgrade answers 101
#          and runs on_open; an unregistered /ws/x answers 404 and runs none;
#          a 600-byte endpoint pattern upgrades;
#        - NULL router / pattern / handler: every registration function
#          returns -1 and writes one DIAGNOSTIC line each.
#   2. C, router.c rebuilt with fault-injecting malloc/realloc/strdup: for
#      every allocation a registration makes (route node, method copy,
#      pattern copy; StaticEntry, pattern buffer; ws pattern copy, ws table;
#      middleware table) failing it makes that call return -1 with a
#      DIAGNOSTIC, leaves nothing half-registered (the path stays 404, no
#      crash, ASan clean), and the same call then succeeds and is reached.
#
# Negative control: C1310_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).  Against a void-returning
# router.h the harness is built with C1310_VOID_API (return codes are not
# checked, reachability still is).
#   fix/z142-router-cap 75ad77ba: 5 passed, 8 failed -- /p16/../p999/ (984
#   prefixes), the second router's prefix and the 600-byte prefix 404; 64 of
#   1000 middleware run and no ETag; /ws/16../ws/999 (984 endpoints, 16
#   on_open) and the 600-byte endpoint 404; the fault-injection part needs
#   the int API.  (Its "returns 0" checks pass vacuously under the void API.)
# Measured on this branch: 21 passed (15 C + 6 fault injection).
#
# Story: 127.349
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C1310_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1310_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1310: std.router's C API has no silent caps and reports failed registrations (127.349)"
echo "----------------------------------------------------------------------"

API=()
grep -q '^int[[:space:]]*router_static' "${S}/router.h" || API=(-DC1310_VOID_API)

# collect OK/BAD lines and sanitizer reports from a harness run
harvest() { # $1 label $2 out $3 err $4 rc
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "$1: ${line#OK }" ;; BAD\ *) fail "$1: ${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "$2")
    if grep -a -q -E 'Sanitizer|runtime error' "$3"; then
        fail "$1: report: $(grep -a -m1 -E 'Sanitizer|runtime error' "$3" | head -c 300)"
    elif [ "$4" -ne 0 ] && ! grep -a -q '^BAD ' "$2"; then
        fail "$1: harness exited $4 after: $(tail -1 "$2" | head -c 200)"
    fi
}

# ── 1. scale: 1000 of each, through the public C API ────────────────────
cat > h.c <<'EOF'
#include "router.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef C1310_VOID_API
#define RC(call) ((call), 0)
#else
#define RC(call) (call)
#endif
#define N 1000
static int bad;
static void ok(int c, const char *what) {
    if (c) printf("OK %s\n", what); else { printf("BAD %s\n", what); bad = 1; }
    fflush(stdout);
}
static TkRouteResp h_hello(TkRouteCtx c) { (void)c; return router_resp_ok("hello", "text/plain"); }
static int mw_runs;
static TkRouteResp mw_count(TkRouteCtx c, TkRouteHandler next) { mw_runs++; return next(c); }
static int opens;
static void on_open(int fd) { (void)fd; opens++; }
static int body_is(TkRouteResp r, const char *want) {
    size_t n = r.body_len ? r.body_len : (r.body ? strlen(r.body) : 0);
    return r.body && n == strlen(want) && memcmp(r.body, want, n) == 0;
}
static int has_header(TkRouteResp r, const char *name) {
    for (uint64_t i = 0; i < r.nheaders; i++)
        if (r.header_names[i] && strcasecmp(r.header_names[i], name) == 0) return 1;
    return 0;
}
/* one WebSocket upgrade over a socketpair: the status code answered */
static int ws_status(TkRouter *r, const char *path) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    char req[1024];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n", path);
    if (write(sv[0], req, strlen(req)) < 0) { close(sv[0]); close(sv[1]); return -1; }
    shutdown(sv[0], SHUT_WR);
    router_handle_fd(r, sv[1]);           /* closes sv[1] */
    char buf[256] = {0};
    ssize_t n = read(sv[0], buf, sizeof buf - 1);
    close(sv[0]);
    int code = -1;
    if (n > 12 && sscanf(buf, "HTTP/1.1 %d", &code) != 1) code = -1;
    return code;
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *base = argv[1];
    static char dirs[N][512], pref[N][32], url[N][48], want[N][16];
    /* ── router_static ── */
    TkRouter *r = router_new();
    int regbad = 0;
    for (int i = 0; i < N; i++) {
        snprintf(dirs[i], sizeof dirs[i], "%s/d%d", base, i);
        mkdir(dirs[i], 0755);
        char f[600]; snprintf(f, sizeof f, "%s/f", dirs[i]);
        FILE *fp = fopen(f, "w"); if (!fp) return 3;
        snprintf(want[i], sizeof want[i], "file%d", i);
        fputs(want[i], fp); fclose(fp);
        snprintf(pref[i], sizeof pref[i], "/p%d/", i);
        snprintf(url[i], sizeof url[i], "/p%d/f", i);
        if (RC(router_static(r, pref[i], dirs[i])) != 0) regbad++;
    }
    ok(regbad == 0, "1000 router_static prefixes: every registration returns 0");
    int miss = 0, first_miss = -1;
    for (int i = 0; i < N; i++) {
        TkRouteResp s = router_dispatch(r, "GET", url[i], NULL, NULL);
        if (!(s.status == 200 && body_is(s, want[i]))) { miss++; if (first_miss < 0) first_miss = i; }
    }
    char msg[160];
    snprintf(msg, sizeof msg, "every one of the 1000 prefixes serves its own file (misses %d, first /p%d/)",
             miss, first_miss);
    ok(miss == 0, msg);
    ok(router_dispatch(r, "GET", "/q/f", NULL, NULL).status == 404
       && router_dispatch(r, "GET", "/p7/none", NULL, NULL).status == 404,
       "an unregistered prefix and a missing file under a registered one: 404");
    TkRouter *r2 = router_new();
    ok(RC(router_static(r2, "/x/", dirs[3])) == 0
       && body_is(router_dispatch(r2, "GET", "/x/f", NULL, NULL), want[3]),
       "a second router's prefix, after 1000 on the first: serves its file");
    /* a 600-byte prefix (the old table truncated it to 511) */
    char lp[640], lu[660];
    lp[0] = '/'; memset(lp + 1, 'a', 598); lp[599] = '/'; lp[600] = 0;
    snprintf(lu, sizeof lu, "%sf", lp);
    ok(RC(router_static(r2, lp, dirs[5])) == 0
       && body_is(router_dispatch(r2, "GET", lu, NULL, NULL), want[5]),
       "a 600-byte prefix: serves its file");
    router_free(r2);
    router_free(r);

    /* ── router_use ── */
    r = router_new();
    router_get(r, "/hello", h_hello);
    regbad = 0;
    for (int i = 0; i < N; i++) if (RC(router_use(r, mw_count)) != 0) regbad++;
    int etag_rc = RC(router_use_etag(r));
    mw_runs = 0;
    TkRouteResp hr = router_dispatch(r, "GET", "/hello", NULL, NULL);
    snprintf(msg, sizeof msg, "1000 middleware: all register and all run on one request (%d ran)", mw_runs);
    ok(regbad == 0 && mw_runs == N && hr.status == 200, msg);
    ok(etag_rc == 0 && has_header(hr, "ETag"), "the 1001st middleware (router_use_etag) runs: ETag present");
    ok(router_dispatch(r, "GET", "/nope", NULL, NULL).status == 404,
       "with 1001 middleware, an unregistered path is still 404");
    router_free(r);

    /* ── router_ws ── */
    r = router_new();
    static char wsp[N][24];
    regbad = 0;
    for (int i = 0; i < N; i++) {
        snprintf(wsp[i], sizeof wsp[i], "/ws/%d", i);
        if (RC(router_ws(r, wsp[i], on_open, NULL, NULL)) != 0) regbad++;
    }
    ok(regbad == 0, "1000 router_ws endpoints: every registration returns 0");
    opens = 0; miss = 0; first_miss = -1;
    for (int i = 0; i < N; i++)
        if (ws_status(r, wsp[i]) != 101) { miss++; if (first_miss < 0) first_miss = i; }
    snprintf(msg, sizeof msg, "every one of the 1000 endpoints upgrades (101) and runs on_open "
             "(misses %d, first /ws/%d, opens %d)", miss, first_miss, opens);
    ok(miss == 0 && opens == N, msg);
    opens = 0;
    ok(ws_status(r, "/ws/x") == 404 && opens == 0, "an unregistered endpoint: 404, no on_open");
    char lw[640]; lw[0] = '/'; memset(lw + 1, 'w', 599); lw[600] = 0;
    ok(RC(router_ws(r, lw, on_open, NULL, NULL)) == 0 && ws_status(r, lw) == 101,
       "a 600-byte endpoint pattern upgrades (101)");
    router_free(r);

#ifndef C1310_VOID_API
    /* ── refused registrations say so ── */
    TkCorsOpts co; memset(&co, 0, sizeof co);
    r = router_new();
    int n1 = 0;
    n1 += router_get(NULL, "/a", h_hello) == -1;
    n1 += router_post(r, NULL, h_hello) == -1;
    n1 += router_put(r, "/a", NULL) == -1;
    n1 += router_delete(NULL, "/a", h_hello) == -1;
    n1 += router_use(r, NULL) == -1;
    n1 += router_use_cors(NULL, co) == -1;
    n1 += router_use_etag(NULL) == -1;
    n1 += router_use_gzip(NULL, 1) == -1;
    n1 += router_static(r, "/s/", NULL) == -1;
    n1 += router_ws(r, NULL, on_open, NULL, NULL) == -1;
    printf("REFUSED %d\n", n1);
    ok(n1 == 10, "NULL router / pattern / handler: all 10 registration functions return -1");
    ok(router_dispatch(r, "PUT", "/a", NULL, NULL).status == 404, "a refused route is not registered (404)");
    router_free(r);
#endif
    return bad;
}
EOF
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CFS=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
LINK=(); FLAGS=()
if LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CFS[@]}")"; then
    while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
    FLAGS+=(-lz)   # router's link flags (stdlib_link_set.sh --flags router); C703 allows only --objects calls
else
    fail "stdlib link set: see stderr above"
fi
if [ "${#LINK[@]}" -gt 0 ]; then
    if ! "${CC}" "${CFS[@]}" ${API[@]+"${API[@]}"} -o h h.c "${LINK[@]}" ${FLAGS[@]+"${FLAGS[@]}"} >h.cc 2>&1; then
        fail "C harness build: $(grep -a -m2 error h.cc | head -c 400)"
    else
        mkdir -p files
        ./h "${WORK}/files" >h.out 2>h.err; rc=$?
        harvest "C" h.out h.err "${rc}"
        if [ ${#API[@]} -eq 0 ]; then
            nd="$(grep -a -c '^DIAGNOSTIC: kind=router_register_failed ' h.err)"
            [ "${nd}" = "10" ] && pass "C: the 10 refusals wrote 10 DIAGNOSTIC lines" \
                || fail "C: refusals wrote ${nd} DIAGNOSTIC lines (want 10)"
        fi
    fi
fi

# ── 2. fault injection: every allocation a registration makes ───────────
cat > fi.c <<'EOF'
#include "router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
/* router.c is built with malloc/realloc/calloc/strdup renamed to these:
 * the fail_at-th allocation from now (1-based) returns NULL; 0 = never. */
static int fail_at, nalloc;
static int fi_tick(void) { nalloc++; return fail_at && nalloc == fail_at; }
void *fi_malloc(size_t n) { return fi_tick() ? NULL : malloc(n); }
void *fi_calloc(size_t a, size_t b) { return fi_tick() ? NULL : calloc(a, b); }
void *fi_realloc(void *p, size_t n) { return fi_tick() ? NULL : realloc(p, n); }
char *fi_strdup(const char *s) { return fi_tick() ? NULL : strdup(s); }
static void arm(int k) { nalloc = 0; fail_at = k; }
static int bad;
static void ok(int c, const char *what) {
    if (c) printf("OK %s\n", what); else { printf("BAD %s\n", what); bad = 1; }
    fflush(stdout);
}
static TkRouteResp h_hello(TkRouteCtx c) { (void)c; return router_resp_ok("hello", "text/plain"); }
static int mw_runs;
static TkRouteResp mw_count(TkRouteCtx c, TkRouteHandler next) { mw_runs++; return next(c); }
static int ws_status(TkRouter *r, const char *path) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    char req[512];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
             "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n\r\n", path);
    if (write(sv[0], req, strlen(req)) < 0) { close(sv[0]); close(sv[1]); return -1; }
    shutdown(sv[0], SHUT_WR);
    router_handle_fd(r, sv[1]);
    char buf[256] = {0};
    ssize_t n = read(sv[0], buf, sizeof buf - 1);
    close(sv[0]);
    int code = -1;
    if (n > 12 && sscanf(buf, "HTTP/1.1 %d", &code) != 1) code = -1;
    return code;
}
/* sweep: fail allocation k = 1, 2, ... of call() until a run makes no
 * failing allocation (the call succeeded with k past its count).  Each
 * failed run must return -1 and leave probe() false; the final run must
 * return 0 and leave probe() true.  Prints the number of failure points. */
typedef int (*Reg)(TkRouter *);
typedef int (*Probe)(TkRouter *);
static void sweep(const char *name, Reg call, Probe probe, int want_min) {
    int nfail = 0, wrong = 0, k;
    char msg[200];
    for (k = 1; k < 32; k++) {
        TkRouter *r = router_new();
        arm(k);
        int rc = call(r);
        int hit = nalloc >= k;
        arm(0);
        if (!hit) {                       /* nothing failed: must have worked */
            if (rc != 0 || !probe(r)) wrong++;
            router_free(r);
            break;
        }
        nfail++;
        if (rc != -1 || probe(r)) wrong++;
        /* the router is still usable: the same call now works */
        if (call(r) != 0 || !probe(r)) wrong++;
        router_free(r);
    }
    snprintf(msg, sizeof msg, "%s: each of its %d allocations failing -> -1, nothing registered, "
             "then the call works (%d wrong)", name, nfail, wrong);
    ok(wrong == 0 && nfail >= want_min && k < 32, msg);
    printf("FAILPOINTS %s %d\n", name, nfail);
}
static char sdir[600];
static int reg_get(TkRouter *r)    { return router_get(r, "/g", h_hello); }
static int pr_get(TkRouter *r)     { return router_dispatch(r, "GET", "/g", NULL, NULL).status == 200; }
static int reg_static(TkRouter *r) { return router_static(r, "/s/", sdir); }
static int pr_static(TkRouter *r)  { return router_dispatch(r, "GET", "/s/f", NULL, NULL).status == 200; }
static int reg_ws(TkRouter *r)     { return router_ws(r, "/w", NULL, NULL, NULL); }
static int pr_ws(TkRouter *r)      { return ws_status(r, "/w") == 101; }
static int reg_use(TkRouter *r)    { int k = fail_at; arm(0); router_get(r, "/g", h_hello); arm(k);
                                     return router_use(r, mw_count); }
static int pr_use(TkRouter *r)     { mw_runs = 0; return router_dispatch(r, "GET", "/g", NULL, NULL).status == 200 && mw_runs > 0; }
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(sdir, sizeof sdir, "%s", argv[1]);
    char f[700]; snprintf(f, sizeof f, "%s/f", sdir);
    FILE *fp = fopen(f, "w"); if (!fp) return 3; fputs("x", fp); fclose(fp);
    sweep("router_get", reg_get, pr_get, 3);        /* node, method, pattern */
    sweep("router_static", reg_static, pr_static, 4); /* entry, pattern, + router_get's 3 */
    sweep("router_ws", reg_ws, pr_ws, 2);           /* pattern copy, table */
    sweep("router_use", reg_use, pr_use, 1);        /* table */
    /* a route with every allocation failing in turn, among working ones:
     * dispatch of the neighbours is unaffected (the old code linked a node
     * with a NULL pattern here and crashed in the matcher) */
    TkRouter *r = router_new();
    router_get(r, "/a", h_hello);
    for (int k = 1; k <= 3; k++) { arm(k); router_get(r, "/b", h_hello); arm(0); }
    router_get(r, "/c", h_hello);
    ok(router_dispatch(r, "GET", "/a", NULL, NULL).status == 200
       && router_dispatch(r, "GET", "/b", NULL, NULL).status == 404
       && router_dispatch(r, "GET", "/c", NULL, NULL).status == 200,
       "routes around three failed registrations dispatch; the failed path is 404");
    router_free(r);
    return bad;
}
EOF
if [ ${#API[@]} -ne 0 ]; then
    fail "fault injection: ${S}/router.h has the void registration API (nothing to check -1 against)"
else
    FCF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
    FLINK=()
    if FL="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --except router.c \
            --cc "${CC}" --objects router -- "${FCF[@]}")"; then
        while IFS= read -r l; do FLINK+=("$l"); done <<< "${FL}"
    else
        fail "fault injection: stdlib link set"
    fi
    if [ "${#FLINK[@]}" -gt 0 ]; then
        if ! "${CC}" "${FCF[@]}" -w -Dmalloc=fi_malloc -Dcalloc=fi_calloc -Drealloc=fi_realloc \
                -Dstrdup=fi_strdup -c -o router_fi.o "${S}/router.c" >fi.cc 2>&1 \
           || ! "${CC}" "${FCF[@]}" -o fi fi.c router_fi.o "${FLINK[@]}" ${FLAGS[@]+"${FLAGS[@]}"} >>fi.cc 2>&1; then
            fail "fault injection: build: $(grep -a -m2 error fi.cc | head -c 400)"
        else
            mkdir -p sfiles
            ./fi "${WORK}/sfiles" >fi.out 2>fi.err; rc=$?
            harvest "fault injection" fi.out fi.err "${rc}"
            nfp=0
            while read -r _ _ n; do nfp=$((nfp + n)); done < <(grep -a '^FAILPOINTS ' fi.out)
            nd="$(grep -a -c '^DIAGNOSTIC: kind=router_register_failed .*reason="out of memory"' fi.err)"
            # every failure point in the sweeps plus the three failed /b routes
            [ "${nd}" -eq $((nfp + 3)) ] && [ "${nfp}" -gt 0 ] \
                && pass "fault injection: each of the $((nfp + 3)) failed registrations wrote its DIAGNOSTIC" \
                || fail "fault injection: ${nd} out-of-memory DIAGNOSTIC lines for $((nfp + 3)) failures"
        fi
    fi
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
