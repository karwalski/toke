#!/usr/bin/env bash
# C722_router_resp_ownership.sh -- the memory std.router's own code allocates
# for a response is owned by the router and released, so serving N requests
# leaves the same number of live allocations as serving 10 (story 121.91).
#
# THE DEFECT, measured on main (e9ec45b5) with the counter below: every
# response the gzip middleware compressed malloc'd two new header arrays and
# the compressed body; every response through the ETag middleware malloc'd
# two header arrays and the ETag string; every router_static response
# malloc'd its body, two header arrays and four header values.  router.h said
# nothing about who frees any of it: the arrays a middleware replaced were
# not freed (they might be a handler's, or CORS's static arrays), and
# router_handle_fd / router_serve, the over-the-wire callers, freed nothing.
# So a server leaked on every such request -- linearly in N.
#
# THE FIX IS AN OWNERSHIP RULE (router.h, "TkRouteResp memory ownership").
# A handler's memory stays the handler's.  What the router's own code
# allocates for a dispatched response is the router's: recorded in one
# response pool, valid until the next dispatch on any router (the window the
# CORS middleware's static arrays already had), freed at that dispatch, by
# router_free, and by router_handle_fd / router_serve as soon as the response
# is written.  The caller frees none of it.  router_static_serve called
# directly keeps its caller-frees contract.
#
# WHAT IS ASSERTED.  A harness serves N requests through (a) a router with
# the gzip and ETag middleware (dispatch_ex, Accept-Encoding: gzip), (b) a
# router_static route behind gzip (dispatch_ex), (c) the gzip+ETag router
# over a socketpair via router_handle_fd.  A dyld-interposed malloc/free
# counter (C019's, Darwin only) reports live allocations at exit for N=10
# and N=200: they must be EQUAL.  The counter is first proven exact on a
# probe with a known number of mallocs, so a zero delta cannot come from a
# counter that sees nothing.  Off Darwin the count is skipped.
#
# Negative control: C722_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib (e9ec45b5): 2 passed (the gate, the run check),
# 1 failed -- the 200-round run ends with 3,610 more live allocations than
# the 10-round run, 19 per round: (a) gzip 3 + ETag 3, (b) static 7 + gzip 3,
# (c) ETag 3 (main's wire path passed no Accept-Encoding, so gzip never ran
# there -- 121.90).  On this branch: 186 and 186.
#
# Story: 121.91
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C722_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
echo "C722: std.router releases the response memory it allocates (121.91)"
echo "----------------------------------------------------------------------"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if [ "$(uname -s)" != "Darwin" ]; then
    echo "  SKIP: the allocation count needs dyld interposition (Darwin only)"
    echo "Results: 0 passed, 0 failed"
    exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c722_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
mkdir -p pub
python3 -c 'import sys; open(sys.argv[1],"w").write("static file line\n" * 64)' pub/a.txt 2>/dev/null \
    || { for i in $(seq 64); do echo "static file line"; done > pub/a.txt; }

# ── the counter (C019's) ──
cat > mc.c <<'MCEOF'
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
static _Atomic uint64_t n_malloc = 0, n_free = 0;
static void mc_report(void) {
    char buf[128];
    int n = snprintf(buf, sizeof buf, "MC %llu %llu\n",
        (unsigned long long)n_malloc, (unsigned long long)n_free);
    write(2, buf, (size_t)n);
}
__attribute__((constructor)) static void mc_init(void) { atexit(mc_report); }
void *mc_malloc(size_t s)            { n_malloc++; return malloc(s); }
void *mc_calloc(size_t n, size_t s)  { n_malloc++; return calloc(n, s); }
void *mc_realloc(void *p, size_t s)  { if (!p) n_malloc++; return realloc(p, s); }
void  mc_free(void *p)               { if (p) n_free++; free(p); }
__attribute__((used)) static struct { const void *repl; const void *orig; }
interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void*)mc_malloc,  (const void*)malloc  },
    { (const void*)mc_calloc,  (const void*)calloc  },
    { (const void*)mc_realloc, (const void*)realloc },
    { (const void*)mc_free,    (const void*)free    },
};
MCEOF
if ! cc -dynamiclib -O2 -o libmc.dylib mc.c 2>mc.err; then
    fail "could not build the allocation counter: $(head -c 300 mc.err)"
    echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1
fi
# live BINARY N -> mallocs minus frees at exit
live() {
    C722_N="$2" DYLD_INSERT_LIBRARIES=./libmc.dylib "$1" "${WORK}/pub" 2>&1 >/dev/null \
        | awk '/^MC /{print $2 - $3}'
}

# ── gate: the counter must see a known number of live allocations ──
cat > probe.c <<'PBEOF'
#include <stdlib.h>
#include <string.h>
int main(void) {
    const char *e = getenv("C722_N");
    long n = e ? atol(e) : 0;
    for (long i = 0; i < n; i++) { void *p = malloc(16); (void)p; char *s = strdup("x"); (void)s; }
    return 0;
}
PBEOF
if ! cc -O0 -o probe probe.c 2>/dev/null; then
    fail "could not build the counter's probe"
else
    p0="$(live ./probe 0)"; p1="$(live ./probe 500)"
    if [ -z "${p0}" ] || [ -z "${p1}" ]; then
        fail "gate: the counter produced no reading (interposition not in effect?)"
    elif [ "$((p1 - p0))" -ne 1000 ]; then
        fail "gate: 1000 known live allocations read as $((p1 - p0))"
    else
        pass "gate: the counter reads 1000 known live allocations as exactly 1000"
    fi
fi

# ── the measured harness (no sanitizer: it would own malloc) ──
CF=(-std=c99 -D_GNU_SOURCE -g -O1 -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

cat > h.c <<'EOF'
#include "router.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static char big[4097];
static TkRouteResp h_big(TkRouteCtx c) { (void)c; return router_resp_ok(big, "text/plain"); }

int main(int argc, char **argv)
{
    (void)argc;
    const char *e = getenv("C722_N");
    long n = e ? atol(e) : 0;
    for (int i = 0; i < 4096; i++) big[i] = "the quick brown fox jumps over the lazy dog\n"[i % 44];

    TkRouter *g = router_new();
    router_get(g, "/t", h_big);
    router_use_gzip(g, 64);
    router_use_etag(g);
    TkRouter *s = router_new();
    router_static(s, "/s", argv[1]);
    router_use_gzip(s, 64);

    const char *hn[] = { "Accept-Encoding" }, *hv[] = { "gzip" };
    static const char req[] = "GET /t HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\n\r\n";
    long seen = 0;
    for (long i = 0; i < n; i++) {
        TkRouteResp a = router_dispatch_ex(g, "GET", "/t", NULL, NULL, hn, hv, 1);
        TkRouteResp b = router_dispatch_ex(s, "GET", "/s/a.txt", NULL, NULL, hn, hv, 1);
        seen += a.status + b.status;
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
        if (write(sv[0], req, sizeof req - 1) != (ssize_t)(sizeof req - 1)) return 2;
        router_handle_fd(g, sv[1]);          /* closes sv[1] */
        char buf[16384];
        while (read(sv[0], buf, sizeof buf) > 0) {}
        close(sv[0]);
    }
    printf("served %ld (status sum %ld)\n", n, seen);
    return 0;
}
EOF
if ! "${CC}" "${CF[@]}" -o h h.c "${LINK[@]}" -lz >h.cc 2>&1; then
    fail "harness build: $(grep -a -m2 error h.cc | head -c 400)"
else
    ok_run="$(C722_N=3 ./h pub 2>&1)"
    case "${ok_run}" in
        "served 3 (status sum 1200)") pass "harness: 3 rounds served, every response 200" ;;
        *) fail "harness run: ${ok_run}" ;;
    esac
    a="$(live ./h 10)"; b="$(live ./h 200)"
    if [ -z "${a}" ] || [ -z "${b}" ]; then
        fail "no allocation reading from the harness"
    elif [ "${a}" -eq "${b}" ]; then
        pass "live allocations at exit: ${a} after 10 rounds, ${b} after 200 (no growth)"
    else
        d=$((b - a))
        fail "live allocations at exit: ${a} after 10 rounds, ${b} after 200 -- ${d} more, $((d / 190)) per round leaked"
    fi
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
