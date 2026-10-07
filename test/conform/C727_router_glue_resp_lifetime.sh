#!/usr/bin/env bash
# C727_router_glue_resp_lifetime.sh -- a std.router response handle
# (router.ok, router.json, ...) is released when its request is done, and a
# handle kept past its request never reaches freed memory (story 121.93).
#
# THE DEFECT, measured on main (7ca0e318) with the counter below.
# router_glue.c's make_resp malloc'd a TkRouterResp for every router.ok /
# html / json / css / text / bad / notfound / status / respond call, and
# router_closure_dispatch read it and freed nothing: one leaked block per
# request, plus one for every response a handler built and did not return.
# A plain free() after the read was not the fix: a program may build a
# response once, in a module `let`, and return it on every request.
#
# THE FIX (router_glue.c, "response handles"), router.h's 121.91 rule
# applied to the handles: a handle made inside a handler is released once
# the dispatch has read the handler's answer, returned or not; a handle
# made outside any dispatch (main, or a module `let` initialised before
# router.serve) lives for the program.  A handle is a slot index plus a
# generation, not a pointer: a handle kept past its request (cached by a
# handler in a mutable global) or an i64 that is no handle at all is
# answered 500 with "DIAGNOSTIC: kind=router_invalid_response" on stderr --
# never a read of freed memory, never a crash.  Slots are reused.
#
# WHAT IS ASSERTED.
#   1. counter gate: a dyld-interposed malloc/free counter (C722's, Darwin
#      only) reads 1000 known live allocations as exactly 1000;
#   2. a C harness registers handlers through the toke ABI wrappers
#      (tk_router_get_w, tk_router_ok_w, ...) and serves N rounds -- a
#      handler returning router.ok, one that builds two responses and
#      returns one, one returning a program-lifetime handle, by
#      router_dispatch and over a socketpair (router_handle_fd).  Live
#      allocations at exit after 10 rounds and after 200 are EQUAL;
#   3. the same harness under ASan/UBSan: each handler's answer is right;
#      the program-lifetime handle answers 200 every time; a handle cached
#      across requests answers 200 then 500 with the diagnostic and no
#      sanitizer report; an i64 that is not a handle answers 500 (main:
#      SIGSEGV); null answers 500; 100 handles in one request, then reuse;
#   4. toke over the wire (router.serve): per-request router.ok, a module
#      `let` response, and a response cached in a mutable global (200, then
#      500 with the diagnostic).
#
# Negative control: C727_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib (7ca0e318): 9 passed, 5 failed -- 950 more
# live allocations after 200 rounds than after 10, 5 per round (/ok 1, /two
# 2, /two over the socket 2); the i64 that is no handle was read as a
# pointer (UBSan: misaligned member access at 0x3039; plain builds SIGSEGV),
# which ends the behaviour run there; the handle kept across requests
# answered 200 again in C and in toke, kept alive by the leak, with no
# diagnostic.  That last is the one deliberate behaviour change: such a
# program now gets 500 on its second request and must build its response
# per request or before router.serve.  On this branch: 18 passed.
#
# Story: 121.93
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C727_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
echo "C727: std.router response handles are released per request (121.93)"
echo "----------------------------------------------------------------------"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c727_XXXXXX")"
trap '[ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null; rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

cat > h.c <<'EOF'
#include "router.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

/* the toke ABI wrappers (router_glue.c) */
int64_t tk_router_new_w(void);
int64_t tk_router_get_w(int64_t router, int64_t path, int64_t handler);
int64_t tk_router_ok_w(int64_t body);
int64_t tk_router_json_w(int64_t body);
int64_t tk_router_text_w(int64_t body);
#define S(x) ((int64_t)(intptr_t)(x))

static int64_t g_perm, g_cached;
static int64_t h_ok(int64_t req)   { (void)req; return tk_router_ok_w(S("ok")); }
static int64_t h_two(int64_t req)  { (void)req; (void)tk_router_json_w(S("dropped")); return tk_router_text_w(S("two")); }
static int64_t h_perm(int64_t req) { (void)req; return g_perm; }
static int64_t h_cache(int64_t req){ (void)req; if (!g_cached) g_cached = tk_router_ok_w(S("cached")); return g_cached; }
static int64_t h_junk(int64_t req) { (void)req; return 12345; }
static int64_t h_null(int64_t req) { (void)req; return 0; }
static int64_t h_many(int64_t req)
{
    (void)req;
    int64_t last = 0;
    for (int i = 0; i < 100; i++) last = tk_router_ok_w(S("many"));
    return last;
}

static int bad;
static void report(int ok, const char *fmt, ...)
{
    va_list ap;
    printf("%s ", ok ? "OK" : "BAD");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    fflush(stdout);
    if (!ok) bad++;
}
static int is(TkRouteResp r, int status, const char *body)
{
    return r.status == status && r.body && strcmp(r.body, body) == 0;
}

int main(int argc, char **argv)
{
    const char *e = getenv("C727_N");
    long n = e ? atol(e) : 0;
    int checks = argc > 1 && strcmp(argv[1], "check") == 0;
    g_perm = tk_router_ok_w(S("perm"));          /* outside any dispatch */
    int64_t ri = tk_router_new_w();
    TkRouter *r = (TkRouter *)(intptr_t)ri;
    tk_router_get_w(ri, S("/ok"),    S(h_ok));
    tk_router_get_w(ri, S("/two"),   S(h_two));
    tk_router_get_w(ri, S("/perm"),  S(h_perm));
    tk_router_get_w(ri, S("/cache"), S(h_cache));
    tk_router_get_w(ri, S("/junk"),  S(h_junk));
    tk_router_get_w(ri, S("/null"),  S(h_null));
    tk_router_get_w(ri, S("/many"),  S(h_many));

    if (checks) {
        TkRouteResp a = router_dispatch(r, "GET", "/ok", NULL, NULL);
        report(is(a, 200, "ok") && a.content_type && strstr(a.content_type, "text/html"),
               "router.ok: 200 ok (%d %s)", a.status, a.body);
        a = router_dispatch(r, "GET", "/two", NULL, NULL);
        report(is(a, 200, "two") && a.content_type && strstr(a.content_type, "text/plain"),
               "two built, one returned: the returned one (%d %s)", a.status, a.body);
        int perm = 1;
        for (int i = 0; i < 3; i++) perm &= is(router_dispatch(r, "GET", "/perm", NULL, NULL), 200, "perm");
        report(perm, "a handle made outside a dispatch answers 200 on every request");
        a = router_dispatch(r, "GET", "/cache", NULL, NULL);
        report(is(a, 200, "cached"), "a handle cached across requests: first request 200 (%d)", a.status);
        a = router_dispatch(r, "GET", "/ok", NULL, NULL);   /* reuses the released slot */
        a = router_dispatch(r, "GET", "/cache", NULL, NULL);
        report(a.status == 500, "... a later request returning it: 500, not freed memory (%d %s)", a.status, a.body ? a.body : "");
        a = router_dispatch(r, "GET", "/junk", NULL, NULL);
        report(a.status == 500, "an i64 that is no handle: 500 (%d)", a.status);
        a = router_dispatch(r, "GET", "/null", NULL, NULL);
        report(a.status == 500, "null: 500 (%d)", a.status);
        a = router_dispatch(r, "GET", "/many", NULL, NULL);
        report(is(a, 200, "many"), "100 handles in one request: 200 (%d)", a.status);
        a = router_dispatch(r, "GET", "/many", NULL, NULL);
        report(is(a, 200, "many"), "... and again, on reused slots: 200 (%d)", a.status);
        return bad ? 1 : 0;
    }

    static const char req[] = "GET /two HTTP/1.1\r\nHost: t\r\n\r\n";
    long seen = 0;
    for (long i = 0; i < n; i++) {
        seen += router_dispatch(r, "GET", "/ok",   NULL, NULL).status;
        seen += router_dispatch(r, "GET", "/two",  NULL, NULL).status;
        seen += router_dispatch(r, "GET", "/perm", NULL, NULL).status;
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
        if (write(sv[0], req, sizeof req - 1) != (ssize_t)(sizeof req - 1)) return 2;
        router_handle_fd(r, sv[1]);          /* closes sv[1] */
        char buf[4096];
        while (read(sv[0], buf, sizeof buf) > 0) {}
        close(sv[0]);
    }
    printf("served %ld (status sum %ld)\n", n, seen);
    return 0;
}
EOF

# ── 3. behaviour, under ASan/UBSan ──
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CFS=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CFS[@]}")" \
    || { fail "stdlib link set (sanitized): see stderr above"; LINKSET=""; }
if [ -n "${LINKSET}" ]; then
    LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
    if ! ASAN_OPTIONS=detect_leaks=0 "${CC}" "${CFS[@]}" -o hs h.c "${LINK[@]}" -lz >hs.cc 2>&1; then
        fail "sanitized harness build: $(grep -a -m2 error hs.cc | head -c 400)"
    else
        ASAN_OPTIONS=detect_leaks=0 ./hs check >hs.out 2>hs.err
        rc=$?
        while IFS= read -r line; do
            case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
        done < <(grep -a -E '^(OK|BAD) ' hs.out)
        if grep -a -q -E 'Sanitizer|runtime error' hs.err; then
            fail "sanitizer report: $(grep -a -m1 -E 'Sanitizer|runtime error' hs.err | head -c 300)"
        elif [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' hs.out; then
            fail "harness exited ${rc} after: $(tail -1 hs.out | head -c 200) $(grep -a -m1 -i -E 'signal|segv|bus' hs.err | head -c 200)"
        fi
        if grep -a -q '^OK .*later request returning it' hs.out; then
            n_diag="$(grep -a -c 'DIAGNOSTIC: kind=router_invalid_response' hs.err)"
            [ "${n_diag}" -ge 2 ] && pass "each invalid answer prints DIAGNOSTIC: kind=router_invalid_response (${n_diag})" \
                || fail "invalid answers printed ${n_diag} router_invalid_response diagnostics (want >= 2)"
        fi
    fi
fi

# ── 1–2. live allocations, Darwin only ──
if [ "$(uname -s)" != "Darwin" ]; then
    echo "  SKIP: the allocation count needs dyld interposition (Darwin only)"
else
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
    live() {
        C727_N="$2" DYLD_INSERT_LIBRARIES=./libmc.dylib "$1" 2>&1 >/dev/null \
            | awk '/^MC /{print $2 - $3}'
    }
    cat > probe.c <<'PBEOF'
#include <stdlib.h>
#include <string.h>
int main(void) {
    const char *e = getenv("C727_N");
    long n = e ? atol(e) : 0;
    for (long i = 0; i < n; i++) { void *p = malloc(16); (void)p; char *s = strdup("x"); (void)s; }
    return 0;
}
PBEOF
    if ! cc -dynamiclib -O2 -o libmc.dylib mc.c 2>mc.err || ! cc -O0 -o probe probe.c 2>/dev/null; then
        fail "could not build the allocation counter: $(head -c 300 mc.err)"
    else
        p0="$(live ./probe 0)"; p1="$(live ./probe 500)"
        if [ -z "${p0}" ] || [ -z "${p1}" ]; then
            fail "gate: the counter produced no reading (interposition not in effect?)"
        elif [ "$((p1 - p0))" -ne 1000 ]; then
            fail "gate: 1000 known live allocations read as $((p1 - p0))"
        else
            pass "gate: the counter reads 1000 known live allocations as exactly 1000"
        fi
        # the measured harness: no sanitizer (it would own malloc)
        CF=(-std=c99 -D_GNU_SOURCE -g -O1 -iquote "${S}")
        LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CF[@]}")" \
            || { fail "stdlib link set: see stderr above"; LINKSET=""; }
        if [ -n "${LINKSET}" ]; then
            LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
            if ! "${CC}" "${CF[@]}" -o h h.c "${LINK[@]}" -lz >h.cc 2>&1; then
                fail "harness build: $(grep -a -m2 error h.cc | head -c 400)"
            else
                ok_run="$(C727_N=3 ./h 2>&1)"
                case "${ok_run}" in
                    "served 3 (status sum 1800)") pass "count harness: 3 rounds served, every dispatched answer 200" ;;
                    *) fail "count harness run: ${ok_run}" ;;
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
        fi
    fi
fi

# ── 4. toke over the wire ──
export TKC_STDLIB_DIR="${S}"
cat > srv.tk <<'TK'
m=srv;
i=router:std.router;
let prebuilt=router.text("prebuilt");
let cached=mut.0;
f=fresh(req:i64):i64{ <router.ok(router.reqpath(req)) };
f=pre(req:i64):i64{ <prebuilt };
f=keep(req:i64):i64{
  if(cached==0){ cached=router.ok("kept") };
  <cached
};
f=main():i64{
  let r=router.new();
  router.get(r;"/a";&fresh);
  router.get(r;"/b";&fresh);
  router.get(r;"/pre";&pre);
  router.get(r;"/keep";&keep);
  <mt router.serve(r;"127.0.0.1";0) {$ok:v 0; $err:e 7}
};
TK
if ! "${TKC}" --diag-text --allow-all --out srv srv.tk > srv.log 2>&1; then
    fail "the toke server does not build: $(grep -m1 '^error' srv.log)"
else
    ./srv >/dev/null 2>srv.err & SRV=$!
    PORT=""
    for _ in $(seq 1 100); do
        PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' srv.err | head -1)"
        [ -n "${PORT}" ] && break
        kill -0 "${SRV}" 2>/dev/null || break
        sleep 0.1
    done
    if [ -z "${PORT}" ]; then
        fail "the toke server did not start: $(head -3 srv.err)"
    else
        get() { curl -s --max-time 5 -o body.txt -w '%{http_code}' "http://127.0.0.1:${PORT}$1" 2>/dev/null; printf ' %s' "$(cat body.txt 2>/dev/null)"; }
        got="$(get /a)|$(get /b)|$(get /a)"
        [ "${got}" = "200 /a|200 /b|200 /a" ] && pass "toke: per-request router.ok answers each request (${got})" \
            || fail "toke: per-request router.ok: ${got}"
        got="$(get /pre)|$(get /pre)|$(get /pre)"
        [ "${got}" = "200 prebuilt|200 prebuilt|200 prebuilt" ] && pass "toke: a module-let response answers every request (${got})" \
            || fail "toke: module-let response: ${got}"
        got="$(get /keep)|$(get /a)|$(get /keep)"
        case "${got}" in
            "200 kept|200 /a|500 "*) pass "toke: a response kept in a global across requests: 200, then 500 (${got})" ;;
            *) fail "toke: a response kept across requests: ${got} (want 200 kept, then 500)" ;;
        esac
        grep -q 'DIAGNOSTIC: kind=router_invalid_response' srv.err \
            && pass "toke: the 500 is explained on stderr (router_invalid_response)" \
            || fail "toke: no router_invalid_response diagnostic on stderr"
        kill -0 "${SRV}" 2>/dev/null && pass "toke: the server is still up" || fail "toke: the server died"
    fi
    kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
