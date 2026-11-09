#!/usr/bin/env bash
# C1070_http_sealed.sh -- std.http's client response header strings are
# sealed, and http.c reads every length with tk_strlen (story 127.295, S1
# step 5 tranche 6; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# The client response's header keys and values are the one http.c producer
# whose strings the program keeps: client_resp_block (tk_web_glue.c) hands
# them to the program as the $httpresp headers.  Every other http.c string is
# owned by C (request fields freed by http_req_free after the handler, the
# client's base_url/proxy_url, CORS origins, the 404 body, SNI names, err_msg
# copied by http_raise) and stays malloc'd.
#
# WHAT IS ASSERTED (a loopback python server serves one canned response):
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen):
#        - http_get over a sealed base URL and path: status 200, every
#          header key and value sealed with header == strlen and the
#          expected text (an empty value too);
#        - tk_http_clientget_w (the toke glue): the [[str]] header pairs it
#          hands out are sealed with the expected text, and building the
#          record frees nothing sealed (an ASan bad-free would abort);
#        - readers over sealed inputs: http_cookie, http_set_cookie_header,
#          http_multipart_boundary, http_etag_fnv give the expected bytes.
#   2. toke, under TK_STR_VERIFY=1: http.clientget's r.headers keys and
#      values have the expected text and `.len`, and the program exits 0.
#      The elements are bound with `let k:str=` because an unannotated
#      `a.get(0)` of a $httpresp header pair interpolates as an integer
#      (story 127.333, a compiler typing bug, not this tranche's).
#
# Negative control: C1070_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   main 3d87016f's src/stdlib + stdlib: 14 passed, 2 failed -- the http_get
#   and the record header strings carry no header ("not sealed"); everything
#   else passes there, because an unsealed string read with strlen was always
#   correct (the reader conversion is a speed change, gated by the ratchet).
# Measured: this branch 16 passed (11 C + 5 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1070_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found (loopback server)"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1070_XXXXXX")"; SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1070: http client response headers sealed; http.c reads with tk_strlen (127.295)"
echo "----------------------------------------------------------------------"

cat > srv.py <<'PY'
import socket, threading
RESP = (b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Sealed-Name: va lue 42\r\n"
        b"X-Empty:\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello")
def serve(c):
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(4096)
            if not d: return
            buf += d
        c.sendall(RESP)
    finally:
        try: c.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        c.close()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(16)
print(s.getsockname()[1], flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
PY
python3 srv.py > srv.port 2> srv.err & SRV=$!
for _ in $(seq 1 50); do [ -s srv.port ] && break; sleep 0.1; done
P="$(head -1 srv.port 2>/dev/null)"
if [ -z "${P}" ]; then echo "FAIL: the loopback server did not start"; echo "C1070: 0 passed, 1 failed"; exit 1; fi
BASE="http://127.0.0.1:${P}"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tk_strhdr.h"
#include "tk_array.h"
#include "http.h"
#include "capabilities.h"
int64_t tk_http_clientget_w(int64_t client, int64_t path);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int sealed_eq(const char *s, const char *want)
{
    if (!s) return 0;
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%s')\n", s); return 0; }
    return (size_t)h == strlen(s) && strcmp(s, want) == 0;
}
static const char *S(const char *s) { return tks_strdup(s); }
static const char *K[] = { "Content-Type", "X-Sealed-Name", "X-Empty", "Content-Length", "Connection" };
static const char *V[] = { "text/plain", "va lue 42", "", "5", "close" };
int main(int argc, char **argv)
{
    char *cargv[] = { argv[0], "--allow-net", NULL };
    tk_cap_init(2, cargv);              /* the client checks the net grant */
    (void)argc;
    /* http_get: the HttpClientResp headers */
    HttpClient *c = http_client(S(argv[1]));
    CHECK(c != NULL, "http_client on a sealed base URL");
    HttpClientResp r = http_get(c, S("/h"));
    CHECK(!r.is_err && r.status == 200, "http_get over a sealed path: status 200");
    CHECK(r.headers.len == 5, "http_get: five response headers");
    int ok = r.headers.len == 5;
    for (uint64_t i = 0; ok && i < 5; i++)
        ok = sealed_eq(r.headers.data[i].key, K[i]) && sealed_eq(r.headers.data[i].val, V[i]);
    CHECK(ok, "http_get: every header key and value sealed, header == strlen (empty value too)");
    free(r.headers.data); free(r.body); free(r.err_msg);

    /* tk_http_clientget_w: the [[str]] pairs handed to the program */
    int64_t rec = tk_http_clientget_w((int64_t)(intptr_t)c, (int64_t)(intptr_t)S("/h"));
    CHECK(rec != 0, "tk_http_clientget_w returns a response record");
    if (rec) {
        TkHttpRespRec *b = (TkHttpRespRec *)(intptr_t)rec;
        CHECK(b->status == 200 && tk_arr_len(b->headers) == 5, "record: status 200, five header pairs");
        int ok2 = tk_arr_len(b->headers) == 5;
        for (int64_t i = 0; ok2 && i < 5; i++) {
            int64_t *pair = (int64_t *)(intptr_t)((int64_t *)(intptr_t)b->headers)[i];
            ok2 = sealed_eq((const char *)(intptr_t)pair[0], K[i]) &&
                  sealed_eq((const char *)(intptr_t)pair[1], V[i]);
        }
        CHECK(ok2, "record: every header pair's key and value sealed, header == strlen");
    }
    http_client_free(c);

    /* readers over sealed inputs */
    const char *ck = http_cookie(S("a=1; session=ab%20cd; z=9"), S("session"));
    CHECK(ck && strcmp(ck, "ab cd") == 0, "http_cookie over a sealed header and name");
    TkCookieOpts o = { S("/app"), S("example.test"), 60, 1, 1, S("Lax") };
    const char *sc = http_set_cookie_header(S("sid"), S("v1"), o);
    CHECK(sc && strcmp(sc, "sid=v1; Path=/app; Domain=example.test; Max-Age=60; Secure; HttpOnly; SameSite=Lax") == 0,
          "http_set_cookie_header over sealed name, value and options");
    const char *bd = http_multipart_boundary(S("multipart/form-data; boundary=xYz-12"));
    CHECK(bd && strcmp(bd, "xYz-12") == 0, "http_multipart_boundary over a sealed content type");
    const char *body = S("payload");
    const char *e1 = http_etag_fnv(body, tk_strlen(body));
    const char *e2 = http_etag_fnv("payload", 7);
    CHECK(e1 && e2 && strcmp(e1, e2) == 0, "http_etag_fnv: a sealed body hashes as its C string");
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects http -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${BASE}" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=http:std.http;
i=args:std.args;
f=show(r:$httpresp):i64{
  io.println("n=\(r.headers.len)");
  let a=r.headers.get(1);
  let b=r.headers.get(2);
  let k1:str=a.get(0);
  let v1:str=a.get(1);
  let k2:str=b.get(0);
  let v2:str=b.get(1);
  io.println("k1=[\(k1)] \(k1.len)");
  io.println("v1=[\(v1)] \(v1.len)");
  io.println("k2=[\(k2)] \(k2.len)");
  io.println("v2=[\(v2)] \(v2.len)");
  <0
};
f=main():i64{
  let c=http.client(args.get(1));
  <mt http.clientget(c;"/h") { $ok:v show(v); $err:e 3 }
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p "${BASE}" > p.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] || fail "toke program exited ${rc}: $(head -c 300 p.out)"
    for want in "n=5" "k1=[X-Sealed-Name] 13" "v1=[va lue 42] 9" "k2=[X-Empty] 7" "v2=[] 0"; do
        if grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}', got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
fi

echo "----------------------------------------------------------------------"
echo "C1070: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
