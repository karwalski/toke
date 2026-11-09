#!/usr/bin/env bash
# C1060_ws_server_dead_handshake.sh -- ws_server.c no longer carries a second
# WebSocket handshake with a wrong RFC 6455 GUID (story 127.332).
#
# ws_server.c defined ws_server_accept_key / ws_server_handshake with
# WS_GUID "258EAFA5-E914-47DA-95CA-5AB5DC11D685"; RFC 6455 §1.3's GUID is
# "258EAFA5-E914-47DA-95CA-C5AB0DC85B11".  Neither function was declared in a
# header or called from anywhere (src/, glue, .tki, tests, ooke, website), so
# they were deleted together with their private SHA-1 and base64 helpers;
# ws.c's ws_accept_key / ws_build_upgrade_response is the one handshake.
#
# WHAT IS ASSERTED:
#   1. No source under src/ carries the wrong GUID, and none defines
#      ws_server_accept_key or ws_server_handshake.
#   2. ws_server.c compiles warning-free (-Wall -Wextra -Werror) and its
#      object exports neither symbol.
#   3. A C harness linked from the std.http + std.ws link set (ws_server.c is
#      in std.http's set) gets the RFC 6455 §1.3 sample from ws_accept_key:
#      "dGhlIHNhbXBsZSBub25jZQ==" -> "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", and
#      ws_build_upgrade_response carries it.
#   4. A toke program importing std.http and std.ws compiles, links and runs.
#
# Negative control: C1060_STDLIB=<dir> uses another tree's src/stdlib.
#   main 034332b8's src/stdlib: 4 passed, 3 failed -- wrong GUID present,
#   both functions defined, both symbols exported; ws.c's handshake and the
#   toke link pass there too (the dead code was never reached).
# Measured: this branch 7 passed, 0 failed.
#
# Story: 127.332
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1060_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1060_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1060: ws_server.c's dead handshake with the wrong GUID is gone (127.332)"
echo "----------------------------------------------------------------------"

# ── 1. sources ───────────────────────────────────────────────────────────────
if grep -rlF --include="*.[ch]" "5AB5DC11D685" "${S}" > bad.txt 2>/dev/null; then
    fail "wrong GUID still in: $(tr '\n' ' ' < bad.txt)"
else
    pass "no source in the stdlib tree carries the wrong GUID"
fi
if grep -rlE --include='*.[ch]' '^(char|int)[[:space:]]*\*?[[:space:]]*ws_server_(accept_key|handshake)[[:space:]]*\(' "${S}" > def.txt 2>/dev/null; then
    fail "ws_server_accept_key / ws_server_handshake still defined in: $(tr '\n' ' ' < def.txt)"
else
    pass "ws_server_accept_key / ws_server_handshake are not defined"
fi

# ── 2. ws_server.c object ────────────────────────────────────────────────────
if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror -iquote "${S}" -I"${S}" \
        -c -o ws_server.o "${S}/ws_server.c" > wss.cc 2>&1; then
    fail "ws_server.c does not compile warning-free"; sed 's/^/        /' wss.cc | head -6
else
    pass "ws_server.c compiles with -Wall -Wextra -Werror"
    if nm ws_server.o 2>/dev/null | grep -E ' T _?ws_server_(accept_key|handshake)$' > sym.txt; then
        fail "ws_server.o exports: $(tr '\n' ' ' < sym.txt)"
    else
        pass "ws_server.o exports neither ws_server_accept_key nor ws_server_handshake"
    fi
fi

# ── 3. the one handshake: ws.c, RFC 6455 §1.3 ────────────────────────────────
cat > hs.c <<'C'
#include <stdio.h>
#include <string.h>
#include "ws.h"
int main(void)
{
    int bad = 0;
    const char *a = ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==");
    if (a && strcmp(a, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0)
        printf("  PASS: ws_accept_key on the RFC 6455 §1.3 sample\n");
    else { printf("  FAIL: ws_accept_key gave '%s'\n", a ? a : "(null)"); bad++; }
    const char *r = ws_build_upgrade_response("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", NULL);
    if (r && strstr(r, "HTTP/1.1 101 ") == r
          && strstr(r, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"))
        printf("  PASS: ws_build_upgrade_response carries the accept key\n");
    else { printf("  FAIL: upgrade response '%s'\n", r ? r : "(null)"); bad++; }
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects, never
# a hand list (C696, C703 case 12).  Built without TK_HAVE_OPENSSL (as C113
# does), so no TLS library is linked.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects http ws -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o hs hs.c "${LS[@]}" -lz -lpthread -lm > hs.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' hs.cc | head -6
else
    ./hs > hs.out 2>&1
    PASS=$((PASS + $(grep -c '^  PASS' hs.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' hs.out)))
    cat hs.out
fi

# ── 4. toke program linking std.http + std.ws ────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=http:std.http;
i=ws:std.ws;
f=main():i64{
  io.println("linked");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke program importing std.http + std.ws did not compile/link"; sed 's/^/        /' p.log | head -6
elif [ "$(./p 2>&1)" = "linked" ]; then
    pass "toke program importing std.http + std.ws links and runs"
else
    fail "toke program output: $(./p 2>&1 | head -c 200)"
fi

echo "----------------------------------------------------------------------"
echo "C1060: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
