#!/usr/bin/env bash
# C464_proxy_owned_headers.sh — the reverse proxy forwards exactly one
# Content-Length and its own X-Forwarded-For, never the client's (story
# 121.18: audit-120 WEB-03).
#
# THE DEFECT, measured on main (a28af1b) with this harness: a client request
# carrying `X-Forwarded-For: 10.0.0.1`, `Forwarded: for=10.0.0.1`,
# `X-Real-IP: 10.0.0.1` and `Content-Length: 5` was forwarded with all four,
# beside the proxy's own X-Forwarded-For and Content-Length -- two
# X-Forwarded-For headers (a backend taking the client's one sees a spoofed
# source) and two Content-Length headers (a desync primitive).  And
# X-Forwarded-Proto was "https" whatever the connection was.
#
# proxy.c is latent: no toke surface reaches proxy_forward today.  The
# harness #includes proxy.c to drive the static proxy_build_request.
#
# Negative control: C464_STDLIB=<dir> points the harness at another stdlib
# tree; with main's proxy.c every check but the control fails.
#
# Story: 121.18

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C464_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c464_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C464: the proxy owns Content-Length and X-Forwarded-*"
echo "------------------------------------------------------"

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.  h.c #includes proxy.c itself.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except proxy.c http)" \
    || { fail "stdlib link set: see stderr above"; echo "C464: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

cat > "${WORK}/h.c" <<'EOF'
#include "proxy.c"
static int bad;
static int count(const char *req, const char *name) {
    int n = 0; size_t l = strlen(name);
    for (const char *p = req; (p = strstr(p, "\r\n")); p += 2)
        if (strncasecmp(p + 2, name, l) == 0 && p[2 + l] == ':') n++;
    return n;
}
static void want(const char *req, const char *name, int n, const char *val) {
    int c = count(req, name);
    int ok = c == n;
    if (ok && val) {
        char needle[256]; snprintf(needle, sizeof needle, "\r\n%s: %s\r\n", name, val);
        ok = strstr(req, needle) != NULL;
    }
    printf("%s %s appears %d time(s) (want %d%s%s)\n", ok ? "OK" : "BAD", name, c, n,
           val ? ", value " : "", val ? val : "");
    if (!ok) bad++;
}
int main(void) {
    StrPair h[] = {
        { "X-Forwarded-For", "10.0.0.1" }, { "Forwarded", "for=10.0.0.1" },
        { "X-Real-IP", "10.0.0.1" }, { "Content-Length", "5" },
        { "X-Forwarded-Proto", "https" }, { "Accept", "text/plain" },
    };
    Req r; memset(&r, 0, sizeof r);
    r.method = "POST"; r.path = "/p"; r.body = "hello";
    r.headers.data = h; r.headers.len = sizeof h / sizeof h[0];
    char *out = proxy_build_request(&r, "backend", 8080, "192.0.2.7");
    if (!out) { puts("BAD no request built"); return 1; }
    want(out, "X-Forwarded-For", 1, "192.0.2.7");
    want(out, "Content-Length", 1, "5");
    want(out, "Forwarded", 0, NULL);
    want(out, "X-Real-IP", 0, NULL);
    want(out, "X-Forwarded-Proto", 0, NULL);
    want(out, "Accept", 1, "text/plain");          /* control */
    free(out);
    return bad ? 1 : 0;
}
EOF

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -Wno-unused-function -iquote "${S}" -I"${S}" \
        -o "${WORK}/h" "${WORK}/h.c" \
        "${LINK[@]}" -lz -lpthread -lm >"${WORK}/cc" 2>&1; then
    fail "build: $(grep -a -m2 error "${WORK}/cc" | head -c 300)"
else
    ASAN_OPTIONS=detect_leaks=0 "${WORK}/h" >"${WORK}/out" 2>&1
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/out")
    grep -a -q -E '^(OK|BAD) ' "${WORK}/out" || fail "harness produced nothing: $(head -c 300 "${WORK}/out")"
fi

echo ""
echo "C464: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
