#!/usr/bin/env bash
# C661_http_setnotfound_serves_unrouted_paths.sh -- http.setnotfound's body is
# what an unrouted path gets, in every serving mode, and it is a copy of the
# caller's string (story 121.62).
#
# THE DEFECT, measured on main (437dc471): tk_web_glue.c's g_notfound_body was
# read only inside tk_static_dispatch, which runs only for a path that matched
# a registered getstatic route.  Every genuinely unknown path got the router's
# plain 9-byte "Not Found", so ooke's templates/errors/404.tkt was never served
# under `ooke serve`.  tk_http_set_notfound also kept the caller's pointer
# rather than a copy.
#
# NOW: http.c holds the body (http_set_notfound strdups it) and the HTTP/1.1,
# TLS and HTTP/2 dispatchers answer an unrouted path with it (404,
# text/html).  A routed path is unchanged, a wrong method on a routed path is
# still 405, and with no setnotfound an unrouted path is still "Not Found".
#
# WHAT IS ASSERTED:
#   per mode (plain single(1), plain workers(2), TLS single, TLS workers,
#   HTTP/2 over TLS when a curl that can negotiate h2 is present -- the
#   server offers h2 in ALPN since 121.80, and every request in that mode must
#   come back as HTTP/2): GET /nope -> 404 with the set body and
#   Content-Type text/html; GET /ok -> 200 "ok"; PUT /ok -> 405;
#   without setnotfound: GET /nope -> 404 "Not Found";
#   copy: a C harness hands tk_http_set_notfound a heap string, scribbles over
#   it and frees it; http_notfound_body() still holds the original text
#   (under ASan when CC carries -fsanitize=address, a stored pointer is a UAF).
#
# Negative control: C661_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib every "unrouted -> set body" case fails (body is
# "Not Found") and the copy harness does not link (main keeps no copy to read).
#
# Story: 121.62
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C661_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c661_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C661: http.setnotfound serves unrouted paths, and copies its body (121.62)"
echo "---------------------------------------------------------------------------"
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

HAVE_TLS=0
if command -v openssl >/dev/null 2>&1 && openssl req -x509 -newkey rsa:2048 -keyout key.pem \
        -out cert.pem -days 2 -nodes -subj "/CN=localhost" > cert.log 2>&1; then
    HAVE_TLS=1
else
    echo "  SKIP: the TLS modes -- no openssl CLI to make a certificate"
fi
CURL=curl

BODY='<h1>custom 404</h1>'
cat > srv.tk <<TOKE
m=srv;
i=http:std.http;
i=args:std.args;
f=main():i64{
  http.getstatic("/ok"; "ok");
  if(args.get(2)=="set"){ http.setnotfound("${BODY}") };
  let mode=args.get(1);
  let r=mut.0;
  if(mode=="workers"){ r=http.serveworkerson("127.0.0.1";0;2) }el{
  if(mode=="single"){ r=http.serveworkerson("127.0.0.1";0;1) }el{
  if(mode=="tlsworkers"){ r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";2) }el{
  r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";1) }}};
  <0
};
TOKE
if ! "${TKC}" --allow-all --out srv_bin srv.tk > build.log 2>&1; then
    echo "  FAIL: the server does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

start() {   # start MODE SET -> sets PORT, SRV
    ./srv_bin "$1" "$2" > out.log 2> err.log &
    SRV=$!; PORT=""
    for _ in $(seq 1 80); do
        PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' err.log | head -1)"
        [ -n "${PORT}" ] && break
        kill -0 "${SRV}" 2>/dev/null || break
        sleep 0.1
    done
}

# mode NAME MODE SCHEME [CURLFLAGS...]
mode() {
    local name="$1" m="$2" scheme="$3"; shift 3
    local why="" code body ct
    start "${m}" set
    if [ -z "${PORT}" ]; then
        fail "${name}: server did not start: $(head -2 err.log | tr '\n' ' ')"
        stop "${SRV}"; SRV=""; return
    fi
    local u="${scheme}://127.0.0.1:${PORT}"
    code="$("${CURL}" -sk "$@" --max-time 5 -o nope.body -D nope.hdr -w '%{http_code}' "${u}/nope")"
    case " $* " in *" --http2 "*)   # 121.80: really HTTP/2, not a fallback
        [ "$(head -1 nope.hdr | cut -d' ' -f1)" = "HTTP/2" ] || why="${why} /nope not HTTP/2: $(head -1 nope.hdr | tr -d '\r');" ;;
    esac
    body="$(cat nope.body 2>/dev/null)"
    ct="$(tr -d '\r' < nope.hdr | grep -i '^content-type:' | head -1)"
    [ "${code}" = 404 ] || why="${why} /nope status ${code};"
    [ "${body}" = "${BODY}" ] || why="${why} /nope body '${body}';"
    case "${ct}" in *text/html*) ;; *) why="${why} /nope ${ct:-no content-type};" ;; esac
    code="$("${CURL}" -sk "$@" --max-time 5 -o ok.body -w '%{http_code}' "${u}/ok")"
    [ "${code}" = 200 ] && [ "$(cat ok.body)" = ok ] || why="${why} /ok gave ${code} '$(cat ok.body)';"
    code="$("${CURL}" -sk "$@" --max-time 5 -o /dev/null -X PUT -w '%{http_code}' "${u}/ok")"
    [ "${code}" = 405 ] || why="${why} PUT /ok gave ${code};"
    stop "${SRV}"; SRV=""
    if [ -z "${why}" ]; then pass "${name}: unrouted -> set body; routed 200; wrong method 405"
    else fail "${name}:${why}"; fi
}

mode "plain single(1)"  single http
mode "plain workers(2)" workers http
if [ "${HAVE_TLS}" = 1 ]; then
    mode "tls single(1)"    tlssingle  https --http1.1
    mode "tls workers(2)"   tlsworkers https --http1.1
    # HTTP/2 needs a curl that offers h2 in ALPN (macOS's SecureTransport
    # build does not); use the first candidate that really negotiates it.
    start tlssingle set
    H2CURL=""
    for c in curl /opt/homebrew/opt/curl/bin/curl /usr/local/opt/curl/bin/curl; do
        command -v "${c}" >/dev/null 2>&1 || continue
        v="$("${c}" -sk --http2 --max-time 5 -o /dev/null -w '%{http_version}' "https://127.0.0.1:${PORT}/ok" 2>/dev/null)"
        if [ "${v}" = 2 ]; then H2CURL="${c}"; break; fi
    done
    stop "${SRV}"; SRV=""
    if [ -n "${H2CURL}" ]; then CURL="${H2CURL}"; mode "http/2 over tls" tlssingle https --http2; CURL=curl
    else echo "  SKIP: HTTP/2 -- no curl here negotiated h2 (one built with OpenSSL and nghttp2 is needed)"; fi
fi

# default unchanged: no setnotfound -> plain "Not Found"
start single unset
if [ -n "${PORT}" ]; then
    code="$(curl -s --max-time 5 -o d.body -w '%{http_code}' "http://127.0.0.1:${PORT}/nope")"
    if [ "${code}" = 404 ] && [ "$(cat d.body)" = "Not Found" ]; then
        pass "no setnotfound: unrouted path is still 404 'Not Found'"
    else fail "no setnotfound: ${code} '$(cat d.body)'"; fi
else fail "no setnotfound: server did not start"; fi
stop "${SRV}"; SRV=""

# the body is copied: the caller's string may be freed right after the call
cat > copy.c <<'C'
#include "http.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int64_t tk_http_set_notfound(int64_t body_ptr);
int main(void)
{
    const char *want = "<p>gone</p>";
    char *b = malloc(64);
    if (!b) return 2;
    strcpy(b, want);
    if (tk_http_set_notfound((int64_t)(intptr_t)b) != 0) return 3;
    memset(b, 'X', 63); b[63] = 0;
    free(b);
    const char *got = http_notfound_body();
    if (!got || strcmp(got, want) != 0) { printf("got '%s'\n", got ? got : "(null)"); return 1; }
    if (tk_http_set_notfound(0) != 0 || http_notfound_body() != NULL) return 4;
    return 0;
}
C
if ${CC} -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-function -g \
        -iquote "${S}" -I"${REPO_ROOT}/stdlib/vendor/tomlc99" -o copy copy.c \
        "${S}/tk_web_glue.c" "${S}/http.c" "${S}/encoding.c" "${S}/str.c" "${S}/log.c" \
        "${S}/capabilities.c" "${S}/auth.c" "${S}/crypto.c" "${S}/dashboard.c" "${S}/router.c" \
        "${S}/task.c" "${S}/template.c" "${S}/toml.c" "${S}/file.c" "${S}/chart.c" "${S}/html.c" "${S}/ws.c" \
        "${S}/tk_runtime.c" "${S}/args.c" "${REPO_ROOT}/stdlib/vendor/tomlc99/toml.c" \
        -lz -lpthread -lm > copy.log 2>&1; then
    if out="$(./copy 2>&1)"; then pass "setnotfound copies the body: it survives the caller's free"
    else fail "setnotfound copy: exit $? ${out}"; fi
else
    fail "setnotfound copy: harness does not build/link: $(grep -m1 -E 'error|undefined' copy.log)"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
