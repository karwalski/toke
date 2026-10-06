#!/usr/bin/env bash
# C463_router_cors_and_decode.sh — the router's CORS middleware never grants
# credentials to "any origin", and its query decoder drops %00 like the
# other two decoders do (story 121.18: audit-120 WEB-05, WEB-06).
#
# THE DEFECTS, measured on main (a28af1b) with this harness:
#
#   WEB-05  allowed_origins={"*"} with allow_credentials=1 ECHOED the
#           caller's Origin and sent Access-Control-Allow-Credentials: true
#           -- on the preflight and on the actual response -- so any site
#           could read credentialed responses.  Now "*" is answered as "*"
#           and Allow-Credentials is never sent beside it (browsers refuse
#           that pair, which is the safe outcome).  A listed origin with
#           credentials is unchanged (control).
#   WEB-06  router_query_get("p=a%00b","p") decoded %00 to a NUL byte, so
#           the value read as "a"; http.c and encoding.c drop %00.  Now "ab".
#
# Both surfaces are the router's C API (router_use_cors, router_query_get);
# std.router's toke surface does not expose them today, so this is latent
# hardening -- and http.setcors, the toke-reachable CORS, never sends
# Allow-Credentials at all.
#
# Negative control: C463_STDLIB=<dir> points the harness at another stdlib
# tree; with main's router.c every WEB-05/WEB-06 check fails.
#
# Story: 121.18

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C463_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c463_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C463: router CORS never pairs * with credentials; query drops %00"
echo "------------------------------------------------------------------"

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" --objects router -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "C463: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

cat > "${WORK}/h.c" <<'EOF'
#include "router.h"
#include <stdio.h>
#include <string.h>
static int bad;
static TkRouteResp h(TkRouteCtx c) { (void)c; return router_resp_ok("hi", "text/plain"); }
static const char *hdr(TkRouteResp r, const char *n) {
    for (uint64_t i = 0; i < r.nheaders; i++)
        if (strcmp(r.header_names[i], n) == 0) return r.header_values[i];
    return NULL;
}
static void check(const char *what, TkRouteResp r, const char *acao, int creds) {
    const char *a = hdr(r, "Access-Control-Allow-Origin");
    const char *c = hdr(r, "Access-Control-Allow-Credentials");
    int ok = a && strcmp(a, acao) == 0 && (creds ? (c != NULL) : (c == NULL));
    printf("%s %s: Allow-Origin=%s Allow-Credentials=%s (want %s, %s)\n",
           ok ? "OK" : "BAD", what, a ? a : "(none)", c ? c : "(none)",
           acao, creds ? "true" : "(none)");
    if (!ok) bad++;
}
static TkRouter *mk(const char **origins, int creds) {
    static const char *m[] = { "GET", NULL };
    TkRouter *r = router_new();
    TkCorsOpts o; memset(&o, 0, sizeof o);
    o.allowed_origins = origins; o.allowed_methods = m;
    o.max_age = -1; o.allow_credentials = creds;
    router_use_cors(r, o);
    router_get(r, "/x", h);
    return r;
}
int main(void) {
    const char *hn[] = { "Origin" }, *hv[] = { "https://evil.example" };
    static const char *star[] = { "*", NULL };
    TkRouter *r = mk(star, 1);
    check("wildcard+credentials GET", router_dispatch_ex(r, "GET", "/x", "", "", hn, hv, 1), "*", 0);
    check("wildcard+credentials preflight", router_dispatch_ex(r, "OPTIONS", "/x", "", "", hn, hv, 1), "*", 0);
    static const char *listed[] = { "https://app.example", NULL };
    const char *hv2[] = { "https://app.example" };
    TkRouter *r2 = mk(listed, 1);
    check("listed origin+credentials GET (control)", router_dispatch_ex(r2, "GET", "/x", "", "", hn, hv2, 1), "https://app.example", 1);
    TkRouter *r3 = mk(star, 0);
    check("wildcard, no credentials (control)", router_dispatch_ex(r3, "GET", "/x", "", "", hn, hv, 1), "*", 0);

    const char *v = router_query_get("p=a%00b&q=%41", "p");
    int ok = v && strcmp(v, "ab") == 0;
    printf("%s query p=a%%00b decodes to \"%s\" (want \"ab\")\n", ok ? "OK" : "BAD", v ? v : "(null)");
    if (!ok) bad++;
    v = router_query_get("p=a%00b&q=%41", "q");
    ok = v && strcmp(v, "A") == 0;
    printf("%s query q=%%41 decodes to \"%s\" (control)\n", ok ? "OK" : "BAD", v ? v : "(null)");
    if (!ok) bad++;
    return bad ? 1 : 0;
}
EOF

if ! "${CC}" "${CF[@]}" -o "${WORK}/h" "${WORK}/h.c" \
        "${LINK[@]}" -lz >"${WORK}/cc" 2>&1; then
    fail "build: $(grep -a -m2 error "${WORK}/cc" | head -c 300)"
else
    ASAN_OPTIONS=detect_leaks=0 "${WORK}/h" >"${WORK}/out" 2>&1
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
    grep -a -q -E '^(OK|BAD) ' "${WORK}/out" || fail "harness produced nothing: $(head -c 300 "${WORK}/out")"
fi

echo ""
echo "C463: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
