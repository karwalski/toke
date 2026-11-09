#!/usr/bin/env bash
# C1270_router_param_dispatch.sh -- a std.router route registered from toke
# with a `:param` or `*` segment reaches its handler, router.param returns the
# bound value, and the request strings toke receives (router.param, reqpath,
# reqquery, reqbody, reqmethod) outlive their request (story 127.343).
#
# THE DEFECT (main 031d72fd).  router_glue.c registers one shared trampoline
# (router_closure_dispatch) with the C router for every toke route, and the
# trampoline found the toke handler by strcmp(registered pattern, request
# path).  router.c's matcher matched `/u/:id` against `/u/12345` and bound
# id=12345, but the trampoline then compared "/u/:id" with "/u/12345", found
# nothing and answered 404: a `:param` or `*` route never ran, so
# router.param could never return a value from toke.
#
# THE SECOND DEFECT, verified here over the wire.  The accessors returned the
# router's own pointers: :param values strdup'd by match_route and freed when
# the dispatch returns; path, query and method in static buffers the next
# request overwrites; the body inside the request buffer (on
# router_handle_fd's stack).  A toke string lives for the program
# (runtime-abi §3), so a handler that keeps one in a mutable global read
# freed memory (ASan: heap-use-after-free) or the next request's bytes.
#
# THE FIX.  router.c records the route its matcher chose for the dispatch in
# progress (router_dispatch_route: router, method, pattern) and the
# trampoline looks the toke handler up by that -- so the router's matcher,
# priorities and param binding decide, and two routers with the same pattern
# keep their own handlers.  The accessors hand toke a sealed copy (tks_strdup,
# runtime-abi §3.1, as 127.295), made once per request and field; reqmethod
# returns the route's method literal.
#
# WHAT IS ASSERTED.
#   1. C, ASan+UBSan, under TK_STR_VERIFY=1, through the toke ABI wrappers
#      (tk_router_get_w ...), by router_dispatch and by router_handle_fd over
#      a socketpair:
#        - GET /u/12345 on /u/:id: 200, the handler sees id=12345;
#        - two params; a `*` route; exact beats :param beats `*`;
#        - two routers, same pattern, different handlers: each its own;
#        - an unmatched path is 404 and runs no handler;
#        - param/path/query/body/method kept in globals across a later
#          request still read their own text, sealed with header == strlen,
#          with no sanitizer report; a repeated read in one request returns
#          the same copy.
#   2. toke over the wire (router.serve on a kernel-chosen loopback port):
#      /u/:id answers the bound id; /p/:a/:b both; /f/* reaches its
#      handler; a param and path kept in mutable globals read back intact
#      after other requests.
#
# Negative control: C1270_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   main 031d72fd: 3 passed, 15 failed -- every :param / `*` route answers
#   404 (C and toke); the body kept from a literal route is an ASan
#   stack-use-after-return (it pointed into router_handle_fd's buffer),
#   which ends the C run.
#   This branch with the dispatch fix but the accessors' copies removed:
#   11 passed, 3 failed -- the kept param is an ASan heap-use-after-free
#   (match_route's strdup, freed after the dispatch), and over the wire the
#   toke server's kept param read back "" and its kept path "/show", the
#   next request's (router_serve's static path buffer).
# Measured on this branch: 19 passed (14 C + 5 toke).
#
# Story: 127.343
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C1270_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found (loopback client)"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1270_XXXXXX")"
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1270: std.router :param / * routes dispatch from toke; request strings outlive the request (127.343)"
echo "----------------------------------------------------------------------"

# ── 1. C harness through the toke ABI wrappers ──────────────────────────
cat > h.c <<'EOF'
#include "router.h"
#include "tk_strhdr.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
int64_t tk_router_new_w(void);
int64_t tk_router_get_w(int64_t router, int64_t path, int64_t handler);
int64_t tk_router_post_w(int64_t router, int64_t path, int64_t handler);
int64_t tk_router_ok_w(int64_t body);
int64_t tk_router_param_w(int64_t req, int64_t name);
int64_t tk_router_reqpath_w(int64_t req);
int64_t tk_router_reqquery_w(int64_t req);
int64_t tk_router_reqbody_w(int64_t req);
int64_t tk_router_reqmethod_w(int64_t req);
#define S(x) ((int64_t)(intptr_t)(x))
#define STR(v) ((const char *)(intptr_t)(v))
static int bad;
static void ok(int c, const char *what) {
    if (c) printf("OK %s\n", what); else { printf("BAD %s\n", what); bad = 1; }
    fflush(stdout);
}
static char last[256];
static const char *k_param, *k_path, *k_query, *k_body, *k_method;
static int same_copy;
static int64_t h_u(int64_t q) {
    const char *v = STR(tk_router_param_w(q, S("id")));
    snprintf(last, sizeof last, "u:%s", v);
    return tk_router_ok_w(S("u"));
}
static int64_t h_me(int64_t q) { (void)q; snprintf(last, sizeof last, "me"); return tk_router_ok_w(S("me")); }
static int64_t h_ab(int64_t q) {
    snprintf(last, sizeof last, "ab:%s,%s", STR(tk_router_param_w(q, S("a"))),
             STR(tk_router_param_w(q, S("b"))));
    return tk_router_ok_w(S("ab"));
}
static int64_t h_star(int64_t q) { snprintf(last, sizeof last, "star:%s", STR(tk_router_reqpath_w(q))); return tk_router_ok_w(S("s")); }
static int64_t h_r2(int64_t q) { snprintf(last, sizeof last, "r2:%s", STR(tk_router_param_w(q, S("id")))); return tk_router_ok_w(S("r2")); }
static int64_t h_keep(int64_t q) {
    k_param  = STR(tk_router_param_w(q, S("id")));
    same_copy = (k_param == STR(tk_router_param_w(q, S("id"))));
    k_path   = STR(tk_router_reqpath_w(q));
    k_query  = STR(tk_router_reqquery_w(q));
    k_body   = STR(tk_router_reqbody_w(q));
    k_method = STR(tk_router_reqmethod_w(q));
    snprintf(last, sizeof last, "keep");
    return tk_router_ok_w(S("kept"));
}
static int sealed_eq(const char *p, const char *want) {
    return p && strcmp(p, want) == 0 && tks_hdrlen(p) == (int64_t)strlen(want);
}
static int wire(TkRouter *r, const char *req) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (write(sv[0], req, strlen(req)) < 0) return -1;
    router_handle_fd(r, sv[1]);
    char buf[4096]; ssize_t n = read(sv[0], buf, sizeof buf - 1);
    close(sv[0]);
    if (n <= 0) return -1;
    buf[n] = 0;
    int st = 0; sscanf(buf, "HTTP/1.1 %d", &st);
    return st;
}
static int disp(int64_t r, const char *path) {
    last[0] = 0;
    return router_dispatch((TkRouter *)(intptr_t)r, "GET", path, NULL, NULL).status;
}
int main(void) {
    int64_t r = tk_router_new_w();
    tk_router_get_w(r, S("/u/:id"), S(h_u));
    tk_router_get_w(r, S("/u/me"), S(h_me));
    tk_router_get_w(r, S("/p/:a/:b"), S(h_ab));
    tk_router_get_w(r, S("/f/*"), S(h_star));
    tk_router_get_w(r, S("/k/:id"), S(h_keep));
    tk_router_post_w(r, S("/k/:id"), S(h_keep));
    tk_router_post_w(r, S("/lit"), S(h_keep));
    int64_t r2 = tk_router_new_w();
    tk_router_get_w(r2, S("/u/:id"), S(h_r2));

    int st = disp(r, "/u/12345");
    ok(st == 200 && !strcmp(last, "u:12345"), ":param route reaches its handler, param bound (GET /u/12345 on /u/:id)");
    st = disp(r, "/p/x/y");
    ok(st == 200 && !strcmp(last, "ab:x,y"), "two params bound (/p/x/y on /p/:a/:b)");
    st = disp(r, "/f/a/b/c");
    ok(st == 200 && !strcmp(last, "star:/f/a/b/c"), "* route reaches its handler (/f/a/b/c on /f/*)");
    st = disp(r, "/u/me");
    ok(st == 200 && !strcmp(last, "me"), "exact /u/me beats /u/:id");
    st = disp(r2, "/u/7");
    ok(st == 200 && !strcmp(last, "r2:7"), "a second router with the same pattern runs its own handler");
    st = disp(r, "/nope/1/2");
    ok(st == 404 && last[0] == 0, "unmatched path: 404, no handler run");

    /* a literal route (dispatched on main too): path, query and body kept
     * past their request */
    st = wire((TkRouter *)(intptr_t)r,
        "POST /lit?a=b HTTP/1.1\r\nHost: t\r\nContent-Length: 8\r\n\r\nlit-body");
    const char *lp = k_path, *lq = k_query, *lb = k_body;
    (void)wire((TkRouter *)(intptr_t)r, "POST /lit?zzzzzzzz=1 HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nxyz");
    ok(st == 200 && sealed_eq(lp, "/lit") && sealed_eq(lq, "a=b") && sealed_eq(lb, "lit-body"),
       "literal route over a socket: kept reqpath/reqquery/reqbody survive the next request (sealed)");
st = wire((TkRouter *)(intptr_t)r,
        "POST /k/4242?x=1&y=2 HTTP/1.1\r\nHost: t\r\nContent-Length: 9\r\n\r\nbody-text");
    ok(st == 200 && same_copy, "over a socket: POST /k/4242 answered 200; a repeated param read returns the same copy");
    /* keep the first request's strings, then serve more requests */
    const char *p1 = k_param, *pa1 = k_path, *q1 = k_query, *b1 = k_body, *m1 = k_method;
    st = wire((TkRouter *)(intptr_t)r, "GET /k/99999999/?q=other HTTP/1.1\r\nHost: t\r\n\r\n");
    ok(st == 200 && sealed_eq(k_param, "99999999"), "over a socket: a later request answered 200 with its own param");
    (void)disp(r, "/u/abcdefgh");
    ok(sealed_eq(p1, "4242"), "kept router.param survives later requests (sealed, header == strlen)");
    ok(sealed_eq(pa1, "/k/4242"), "kept reqpath survives later requests (sealed)");
    ok(sealed_eq(q1, "x=1&y=2"), "kept reqquery survives later requests (sealed)");
    ok(sealed_eq(b1, "body-text"), "kept reqbody survives later requests (sealed)");
    ok(m1 && !strcmp(m1, "POST"), "kept reqmethod survives later requests");
    return bad;
}
EOF
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CFS=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:detect_stack_use_after_return=1"
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CFS[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
if [ -n "${LINKSET}" ]; then
    LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
    FLAGS=(); while IFS= read -r l; do [ -n "$l" ] && FLAGS+=("$l"); done \
        < <("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --flags router)
    if ! "${CC}" "${CFS[@]}" -o h h.c "${LINK[@]}" "${FLAGS[@]}" >h.cc 2>&1; then
        fail "C harness build: $(grep -a -m2 error h.cc | head -c 400)"
    else
        TK_STR_VERIFY=1 ./h >h.out 2>h.err; rc=$?
        while IFS= read -r line; do
            case "${line}" in OK\ *) pass "C: ${line#OK }" ;; BAD\ *) fail "C: ${line#BAD }" ;; esac
        done < <(grep -a -E '^(OK|BAD) ' h.out)
        if grep -a -q -E 'Sanitizer|runtime error|TK_STR_VERIFY' h.err; then
            fail "C: report: $(grep -a -m1 -E 'Sanitizer|runtime error|TK_STR_VERIFY' h.err | head -c 300)"
        elif [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' h.out; then
            fail "C: harness exited ${rc} after: $(tail -1 h.out | head -c 200)"
        fi
    fi
fi

# ── 2. toke over the wire ──────────────────────────────────────────────
export TKC_STDLIB_DIR="${S}"
cat > srv.tk <<'TK'
m=srv;
i=router:std.router;
i=str:std.str;
let kp=mut."";
let kpath=mut."";
f=u(req:i64):i64{ <router.text(str.concat("id=";router.param(req;"id"))) };
f=ab(req:i64):i64{ <router.text(str.concat(router.param(req;"a");str.concat("/";router.param(req;"b")))) };
f=star(req:i64):i64{ <router.text(str.concat("star ";router.reqpath(req))) };
f=keep(req:i64):i64{
  kp=router.param(req;"id");
  kpath=router.reqpath(req);
  <router.text("kept")
};
f=show(req:i64):i64{ <router.text(str.concat(kp;str.concat("|";kpath))) };
f=main():i64{
  let r=router.new();
  router.get(r;"/u/:id";&u);
  router.get(r;"/p/:a/:b";&ab);
  router.get(r;"/f/*";&star);
  router.get(r;"/keep/:id";&keep);
  router.get(r;"/show";&show);
  <mt router.serve(r;"127.0.0.1";0) {$ok:v 0; $err:e 7}
};
TK
if ! "${TKC}" --diag-text --allow-all --out srv srv.tk > srv.log 2>&1; then
    fail "toke: the server does not build: $(grep -m1 -i 'error' srv.log | head -c 300)"
else
    TK_STR_VERIFY=1 ./srv >/dev/null 2>srv.err & SRV=$!
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
        got="$(get /u/12345)"
        [ "${got}" = "200 id=12345" ] && pass "toke: /u/:id answers router.param (${got})" || fail "toke: /u/12345: ${got} (want 200 id=12345)"
        got="$(get /p/left/right)"
        [ "${got}" = "200 left/right" ] && pass "toke: two params (${got})" || fail "toke: /p/left/right: ${got}"
        got="$(get /f/x/y)"
        [ "${got}" = "200 star /f/x/y" ] && pass "toke: a * route reaches its handler (${got})" || fail "toke: /f/x/y: ${got}"
        got="$(get /keep/4242)|$(get /u/99999999)|$(get /p/aaaaaaaa/bbbbbbbb)|$(get /show)"
        [ "${got}" = "200 kept|200 id=99999999|200 aaaaaaaa/bbbbbbbb|200 4242|/keep/4242" ] \
            && pass "toke: a param and path kept in mutable globals read back intact after later requests" \
            || fail "toke: kept strings: ${got} (want ...|200 4242|/keep/4242)"
        kill -0 "${SRV}" 2>/dev/null && pass "toke: the server is still up" || fail "toke: the server died: $(grep -a -m1 -E 'TK_STR_VERIFY|signal' srv.err)"
    fi
    kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
