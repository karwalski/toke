#!/usr/bin/env bash
# C1290_router_route_table_grows.sh -- std.router accepts and dispatches any
# number of toke routes: the 129th and the 1000th route registered reach
# their own handlers (story 127.348).
#
# THE DEFECT (fix/z140-router-params 3f76d385; same logic on main 031d72fd).
# router_glue.c kept each toke route's {router, method, pattern, handler} in a
# fixed g_closure_routes[128].  When it was full, register_route skipped the
# entry but still registered the shared trampoline with the C router and
# returned 0 (success).  The route then matched in router.c, the trampoline
# found no handler for it and answered 404 -- no diagnostic, no error.
# Measured: /r0../r129 all register -> 0; /r127 200, /r128 404, /r129 404.
#
# THE FIX.  The glue's table grows (doubling realloc).  If growing it fails,
# the route is not registered with the C router, registration returns -1 and
# stderr carries DIAGNOSTIC: kind=router_register_failed -- a route is never
# accepted without a handler.  router.c's own route table is a linked list
# with no cap (checked; nothing to fix there).
#
# WHAT IS ASSERTED.
#   1. C, ASan+UBSan, through the toke ABI wrappers (tk_router_get_w ...):
#        - 1000 GET routes /r0../r999 on one router: every registration
#          returns 0; every one dispatches 200 to the handler that sees its
#          own path -- including the 128/129 boundary (/r127, /r128) and the
#          last (/r999);
#        - 300 POST routes on a second router, registered after: each 200,
#          and router 1 keeps its own;
#        - a :param route registered after 1300 others binds its param;
#        - an unregistered path is still 404 and runs no handler.
#   2. toke over the wire (router.serve on a kernel-chosen loopback port):
#      a program registering 200 routes answers /r0, /r127, /r128, /r129
#      and /r199 with 200 and its own path.
#
# Negative control: C1290_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   fix/z140-router-params 3f76d385: 7 passed, 10 failed -- /r128, /r129,
#   /r999, the second router's routes and the late :param route all answer
#   404 in C, and /r128, /r129, /r199 answer 404 over the wire.
# Measured on this branch: 17 passed (11 C + 6 toke).
#
# Story: 127.348
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C1290_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found (loopback client)"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1290_XXXXXX")"
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1290: std.router dispatches every toke route, past the old 128-route table (127.348)"
echo "----------------------------------------------------------------------"

# ── 1. C harness through the toke ABI wrappers ──────────────────────────
cat > h.c <<'EOF'
#include "router.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int64_t tk_router_new_w(void);
int64_t tk_router_get_w(int64_t router, int64_t path, int64_t handler);
int64_t tk_router_post_w(int64_t router, int64_t path, int64_t handler);
int64_t tk_router_ok_w(int64_t body);
int64_t tk_router_param_w(int64_t req, int64_t name);
int64_t tk_router_reqpath_w(int64_t req);
#define S(x) ((int64_t)(intptr_t)(x))
#define STR(v) ((const char *)(intptr_t)(v))
#define NG 1000
#define NP 300
static int bad;
static void ok(int c, const char *what) {
    if (c) printf("OK %s\n", what); else { printf("BAD %s\n", what); bad = 1; }
    fflush(stdout);
}
static char last[256];
static int64_t h_get(int64_t q)  { snprintf(last, sizeof last, "g:%s", STR(tk_router_reqpath_w(q))); return tk_router_ok_w(S("g")); }
static int64_t h_post(int64_t q) { snprintf(last, sizeof last, "p:%s", STR(tk_router_reqpath_w(q))); return tk_router_ok_w(S("p")); }
static int64_t h_id(int64_t q)   { snprintf(last, sizeof last, "id:%s", STR(tk_router_param_w(q, S("id")))); return tk_router_ok_w(S("i")); }
static char gp[NG][16], pp[NP][16];
static int disp(int64_t r, const char *m, const char *path) {
    last[0] = 0;
    return router_dispatch((TkRouter *)(intptr_t)r, m, path, NULL, NULL).status;
}
static int hits(int64_t r, const char *m, const char *path, const char *tag) {
    char want[64];
    snprintf(want, sizeof want, "%s:%s", tag, path);
    return disp(r, m, path) == 200 && strcmp(last, want) == 0;
}
int main(void) {
    int64_t r = tk_router_new_w();
    int regbad = 0;
    for (int i = 0; i < NG; i++) {
        snprintf(gp[i], sizeof gp[i], "/r%d", i);
        if (tk_router_get_w(r, S(gp[i]), S(h_get)) != 0) regbad++;
    }
    ok(regbad == 0, "1000 GET routes: every registration returns 0");
    ok(hits(r, "GET", "/r0", "g"), "first route /r0: 200, its handler");
    ok(hits(r, "GET", "/r127", "g"), "128th route /r127: 200, its handler");
    ok(hits(r, "GET", "/r128", "g"), "129th route /r128 (past the old table): 200, its handler");
    ok(hits(r, "GET", "/r129", "g"), "130th route /r129: 200, its handler");
    ok(hits(r, "GET", "/r999", "g"), "1000th route /r999: 200, its handler");
    int miss = 0;
    for (int i = 0; i < NG; i++) if (!hits(r, "GET", gp[i], "g")) miss++;
    ok(miss == 0, "all 1000 GET routes dispatch to their handler");

    int64_t r2 = tk_router_new_w();
    regbad = 0;
    for (int i = 0; i < NP; i++) {
        snprintf(pp[i], sizeof pp[i], "/r%d", i);
        if (tk_router_post_w(r2, S(pp[i]), S(h_post)) != 0) regbad++;
    }
    miss = 0;
    for (int i = 0; i < NP; i++) if (!hits(r2, "POST", pp[i], "p")) miss++;
    ok(regbad == 0 && miss == 0, "a second router's 300 POST routes, registered after: each 200, its handler");
    ok(hits(r, "GET", "/r200", "g"), "router 1 keeps its own handler for /r200");
    ok(tk_router_get_w(r, S("/u/:id"), S(h_id)) == 0 && disp(r, "GET", "/u/4242") == 200
       && strcmp(last, "id:4242") == 0, ":param route registered after 1300 others binds its param");
    ok(disp(r, "GET", "/nope") == 404 && last[0] == 0, "unregistered path: 404, no handler run");
    return bad;
}
EOF
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CFS=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CFS[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
if [ -n "${LINKSET}" ]; then
    LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
    FLAGS=(-lz)   # router's link flags (stdlib_link_set.sh --flags router); C703 allows only --objects calls
    if ! "${CC}" "${CFS[@]}" -o h h.c "${LINK[@]}" "${FLAGS[@]}" >h.cc 2>&1; then
        fail "C harness build: $(grep -a -m2 error h.cc | head -c 400)"
    else
        ./h >h.out 2>h.err; rc=$?
        while IFS= read -r line; do
            case "${line}" in OK\ *) pass "C: ${line#OK }" ;; BAD\ *) fail "C: ${line#BAD }" ;; esac
        done < <(grep -a -E '^(OK|BAD) ' h.out)
        if grep -a -q -E 'Sanitizer|runtime error' h.err; then
            fail "C: report: $(grep -a -m1 -E 'Sanitizer|runtime error' h.err | head -c 300)"
        elif [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' h.out; then
            fail "C: harness exited ${rc} after: $(tail -1 h.out | head -c 200)"
        fi
    fi
fi

# ── 2. toke over the wire: 200 routes ──────────────────────────────────
export TKC_STDLIB_DIR="${S}"
{
    printf 'm=srv;\ni=router:std.router;\ni=str:std.str;\n'
    printf 'f=h(req:i64):i64{ <router.text(str.concat("hit ";router.reqpath(req))) };\n'
    printf 'f=main():i64{\n  let r=router.new();\n'
    for i in $(seq 0 199); do printf '  router.get(r;"/r%d";&h);\n' "$i"; done
    printf '  <mt router.serve(r;"127.0.0.1";0) {$ok:v 0; $err:e 7}\n};\n'
} > srv.tk
if ! "${TKC}" --diag-text --allow-all --out srv srv.tk > srv.log 2>&1; then
    fail "toke: the server does not build: $(grep -m1 -i 'error' srv.log | head -c 300)"
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
        fail "toke: the server did not start: $(head -3 srv.err)"
    else
        get() { curl -s --max-time 5 -o body.txt -w '%{http_code}' "http://127.0.0.1:${PORT}$1" 2>/dev/null; printf ' %s' "$(cat body.txt 2>/dev/null)"; }
        for p in /r0 /r127 /r128 /r129 /r199; do
            got="$(get "${p}")"
            [ "${got}" = "200 hit ${p}" ] && pass "toke: 200 routes registered, ${p} answers (${got})" \
                || fail "toke: ${p}: ${got} (want 200 hit ${p})"
        done
        got="$(get /r200)"
        [ "${got%% *}" = "404" ] && pass "toke: unregistered /r200 is 404" || fail "toke: /r200: ${got} (want 404)"
    fi
    kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
