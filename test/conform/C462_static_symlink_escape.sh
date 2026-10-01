#!/usr/bin/env bash
# C462_static_symlink_escape.sh — neither static file server follows a
# symlink out of the directory it serves (story 121.18: audit-120 WEB-02).
#
# THE DEFECTS, measured on main (a28af1b) with this script:
#
#   http.servedir       its only traversal check was strstr("..") on the
#                       joined path.  A symlink inside the served tree that
#                       points outside it was followed: GET /s/link/secret.txt
#                       through `link -> <outside>` answered 200 with the
#                       outside file, and so did a file symlink and a
#                       directory whose index.html was a symlink out.
#   router_static_serve realpath()-checked the requested path but NOT the
#                       index.html it appends for a directory, so the same
#                       index.html symlink served the outside file there too.
#
# A symlink that stays inside the root must keep working (control).
#
# Part 1 is end to end: a toke server built by tkc with http.servedir, driven
# with curl.  Part 2 drives router_static_serve directly from C under ASan.
#
# Negative control: C462_STDLIB=<dir> runs part 2 against another stdlib
# tree (main's router.c fails the index case); for part 1, run this script
# with TKC pointing at a compiler whose stdlib dir holds main's
# tk_web_glue.c -- every escape case answers 200.
#
# Story: 121.18

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C462_STDLIB:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c462_XXXXXX")"
SRV=""
cleanup() {
    [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "C462: static serving does not follow symlinks out of its root"
echo "--------------------------------------------------------------"

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" router)" \
    || { fail "stdlib link set: see stderr above"; echo "C462: ${PASS} passed, ${FAIL} failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"

# ── the tree ──────────────────────────────────────────────────────────────
mkdir -p "${WORK}/outside" "${WORK}/root/s/sub" "${WORK}/root/s/inner"
echo "SECRET" > "${WORK}/outside/secret.txt"
echo "PUBLIC" > "${WORK}/root/s/ok.txt"
echo "INNER"  > "${WORK}/root/s/inner/index.html"
ln -s "${WORK}/outside"            "${WORK}/root/s/link"
ln -s "${WORK}/outside/secret.txt" "${WORK}/root/s/leak.txt"
ln -s "${WORK}/outside/secret.txt" "${WORK}/root/s/sub/index.html"
ln -s ok.txt                       "${WORK}/root/s/alias.txt"

# ── part 1: http.servedir, end to end ────────────────────────────────────
if [ ! -x "${TKC}" ]; then
    fail "tkc binary not found at ${TKC} (run 'make' first)"
else
    cat > "${WORK}/srv.tk" <<EOF
m=srv;
i=http:std.http;
f=main():i64{
  http.servedir("/s";"${WORK}/root");
  http.serveworkers(0;1);
  <0
};
EOF
    if ! (cd "${WORK}" && "${TKC}" --allow-all -o srv.bin srv.tk >build.log 2>&1); then
        fail "servedir server did not build: $(head -c 300 "${WORK}/build.log")"
    else
        "${WORK}/srv.bin" >/dev/null 2>"${WORK}/srv.err" &
        SRV=$!
        PORT=""
        for _ in $(seq 1 80); do
            PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "${WORK}/srv.err" | head -1)"
            [ -n "${PORT}" ] && break
            kill -0 "${SRV}" 2>/dev/null || break
            sleep 0.1
        done
        if [ -z "${PORT}" ]; then
            fail "servedir server never came up"
        else
            check() {  # $1 path  $2 "200:<body>" or "deny"
                local code body
                body="$(curl -s --max-time 10 -w '\n%{http_code}' "http://127.0.0.1:${PORT}$1")"
                code="${body##*$'\n'}"; body="${body%$'\n'*}"; body="$(printf '%s' "${body}" | tr -d '\n')"
                if [ "$2" = "deny" ]; then
                    if [ "${code}" = "403" ] || [ "${code}" = "404" ]; then
                        pass "servedir GET $1 -> ${code}"
                    else
                        fail "servedir GET $1 -> ${code} [${body}] -- followed a symlink out of the root"
                    fi
                elif [ "${code}:${body}" = "$2" ]; then
                    pass "servedir GET $1 -> ${code} [${body}]"
                else
                    fail "servedir GET $1 -> ${code} [${body}], want $2"
                fi
            }
            check /s/ok.txt           "200:PUBLIC"
            check /s/alias.txt        "200:PUBLIC"
            check /s/inner/           "200:INNER"
            check /s/link/secret.txt  deny
            check /s/leak.txt         deny
            check /s/sub/             deny
            check /s/sub              deny
        fi
        kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""
    fi
fi

# ── part 2: router_static_serve, from C ──────────────────────────────────
cat > "${WORK}/r.c" <<'EOF'
#include "router.h"
#include <stdio.h>
#include <string.h>
static int bad;
static void want(const char *root, const char *rel, int status, const char *body) {
    TkRouteResp r = router_static_serve(root, rel, NULL, NULL);
    int ok = (r.status == status) &&
             (!body || (r.body && strncmp(r.body, body, strlen(body)) == 0));
    printf("%s router_static_serve \"%s\" -> %d (want %d)\n",
           ok ? "OK" : "BAD", rel, r.status, status);
    if (!ok) bad++;
}
int main(int argc, char **argv) {
    (void)argc;
    const char *root = argv[1];
    want(root, "ok.txt", 200, "PUBLIC");
    want(root, "alias.txt", 200, "PUBLIC");
    want(root, "inner/", 200, "INNER");
    want(root, "link/secret.txt", 403, NULL);
    want(root, "leak.txt", 403, NULL);
    want(root, "sub/", 403, NULL);
    return bad ? 1 : 0;
}
EOF
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! ASAN_OPTIONS=detect_leaks=0 "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" \
        -o "${WORK}/r" "${WORK}/r.c" "${LINK[@]}" -lz >"${WORK}/r.cc" 2>&1; then
    fail "router harness build: $(grep -a -m2 error "${WORK}/r.cc" | head -c 300)"
else
    ASAN_OPTIONS=detect_leaks=0 "${WORK}/r" "${WORK}/root/s" >"${WORK}/r.out" 2>&1
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' "${WORK}/r.out")
    grep -a -q -E '^(OK|BAD) ' "${WORK}/r.out" || fail "router harness produced nothing: $(head -c 300 "${WORK}/r.out")"
fi

echo ""
echo "C462: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
