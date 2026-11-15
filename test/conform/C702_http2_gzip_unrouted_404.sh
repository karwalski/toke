#!/usr/bin/env bash
# C702_http2_gzip_unrouted_404.sh -- HTTP/2 responses are gzipped by the rules
# HTTP/1.1 uses, and a method no route takes on a path only the "*" fallback
# covers is 404, not 405 (story 121.87).
#
# THE DEFECTS, measured on main (ce75d43c):
#   1. The HTTP/1.1 loops (cleartext and TLS) gzip a 2xx body of at least 256
#      bytes when the request's Accept-Encoding names gzip; h2_dispatch never
#      did, so the same server sent style.css gzipped over HTTP/1.1 on TLS and
#      whole over HTTP/2 -- to browsers, which all negotiate h2.
#   2. An http.servedir mount registers GET "*", and "*" matches every path,
#      so the 405 rule ("the path has a route, but not for this method") fired
#      for every non-GET to every unrouted path: PUT /elsewhere was 405 Method
#      Not Allowed on all three transports once any mount existed.
#
# NOW: h2_dispatch runs maybe_gzip_body per stream and sends content-encoding
# gzip and vary; HEAD keeps the unencoded length, as on HTTP/1.1.  A "*" route
# is a fallback: it does not make a path routed for the 405 rule, so 405 is
# answered only when a route naming the path exists for another method.  A
# non-GET under a mount is therefore 404 too: a mount serves GET and HEAD, and
# the file it would name is not a route.
#
# WHAT IS ASSERTED, against a real tkc-built server (one worker; getstatic
# "/ok" and a servedir mount at /s):
#   gzip, over HTTP/2 and (parity) HTTP/1.1 on TLS:
#     - /s/big.css (4 KiB of text) with --compressed: content-encoding gzip,
#       decodes to the file;
#     - /s/big.css without Accept-Encoding: no content-encoding, the file;
#     - /s/small.txt (under 256 B) with --compressed: not encoded;
#   404 vs 405, over HTTP/1.1, HTTP/1.1 on TLS and HTTP/2:
#     - PUT /elsewhere and DELETE /elsewhere -> 404;
#     - POST /s/small.txt -> 404;
#     - POST /ok -> 405 (a route names /ok, for GET);
#     - GET /elsewhere -> 404; GET /ok -> 200.
# TLS cases need the openssl CLI, HTTP/2 cases a curl that can negotiate h2;
# each prints a named SKIP without.
#
# Negative control: C702_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib the HTTP/2 --compressed case is unencoded, and
# every PUT/DELETE/POST-to-unrouted case is 405.
#
# Story: 121.87
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C702_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c702_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C702: HTTP/2 gzip parity; an unrouted non-GET is 404, not 405 (121.87)"
echo "-------------------------------------------------------------------------"
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

mkdir -p pub/s
for i in $(seq 1 128); do printf 'body { margin: %d; }  /* line */\n' "${i}"; done | head -c 4096 > pub/s/big.css
printf 'small\n' > pub/s/small.txt

HAVE_TLS=0
if command -v openssl >/dev/null 2>&1 && openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
        -days 2 -nodes -subj "/CN=localhost" > cert.log 2>&1; then HAVE_TLS=1; fi

cat > srv.tk <<TOKE
m=srv;
i=http:std.http;
i=args:std.args;
f=main():i64{
  http.getstatic("/ok"; "ok");
  http.servedir("/s";"${WORK}/pub");
  let r=mut.0;
  if((mt args.get(1){\$ok:v v;\$err:e ""})=="tls"){ r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";1) }el{
  r=http.serveworkerson("127.0.0.1";0;1) };
  <0
};
TOKE
if ! "${TKC}" --allow-all --out srv_bin srv.tk > build.log 2>&1; then
    echo "  FAIL: the server does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

start() {   # start MODE -> sets PORT, SRV
    ./srv_bin "$1" > out.log 2> err.log &
    SRV=$!; PORT=""
    for _ in $(seq 1 80); do
        PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' err.log | head -1)"
        [ -n "${PORT}" ] && break
        kill -0 "${SRV}" 2>/dev/null || break
        sleep 0.1
    done
}

H2CURL=""
for c in curl /opt/homebrew/opt/curl/bin/curl /usr/local/opt/curl/bin/curl; do
    command -v "${c}" >/dev/null 2>&1 || continue
    v="$("${c}" -V 2>/dev/null)"
    case "${v}" in *SecureTransport*) continue ;; esac
    case "${v}" in *OpenSSL*|*LibreSSL*|*BoringSSL*|*quictls*) ;; *) continue ;; esac
    if printf '%s\n' "${v}" | grep -q '^Features:.*HTTP2'; then H2CURL="${c}"; break; fi
done

hdr() { tr -d '\r' < "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//'; }

# gzip_cases LABEL CURL BASE WANT-VERSION CURL-OPTS...
gzip_cases() {
    local label="$1" cu="$2" u="$3" wantv="$4"; shift 4
    local r ce
    r="$("${cu}" -sk "$@" --compressed --max-time 10 -o got.css -D got.hdr -w '%{http_version} %{http_code}' "${u}/s/big.css")"
    ce="$(hdr got.hdr content-encoding)"
    if [ "${r}" = "${wantv} 200" ] && [ "${ce}" = gzip ] && cmp -s got.css pub/s/big.css; then
        pass "${label}: --compressed big.css is gzip and decodes to the file"
    else fail "${label}: --compressed big.css: '${r}' content-encoding '${ce}'$(cmp -s got.css pub/s/big.css || echo ', bytes differ') (want '${wantv} 200' gzip)"; fi
    r="$("${cu}" -sk "$@" --max-time 10 -o got.css -D got.hdr -w '%{http_code} %{size_download}' "${u}/s/big.css")"
    ce="$(hdr got.hdr content-encoding)"
    if [ "${r}" = "200 4096" ] && [ -z "${ce}" ] && cmp -s got.css pub/s/big.css; then
        pass "${label}: no Accept-Encoding -> big.css unencoded"
    else fail "${label}: plain big.css: '${r}' content-encoding '${ce}'"; fi
    r="$("${cu}" -sk "$@" --compressed --max-time 10 -o got.txt -D got.hdr -w '%{http_code}' "${u}/s/small.txt")"
    ce="$(hdr got.hdr content-encoding)"
    if [ "${r}" = 200 ] && [ -z "${ce}" ] && cmp -s got.txt pub/s/small.txt; then
        pass "${label}: small.txt (under 256 B) not encoded"
    else fail "${label}: small.txt: '${r}' content-encoding '${ce}'"; fi
}

# method_cases LABEL CURL BASE CURL-OPTS...
method_cases() {
    local label="$1" cu="$2" u="$3"; shift 3
    local want m p c why=""
    for spec in "PUT /elsewhere 404" "DELETE /elsewhere 404" "POST /s/small.txt 404" \
                "POST /ok 405" "GET /elsewhere 404" "GET /ok 200"; do
        m="${spec%% *}"; p="${spec#* }"; want="${p#* }"; p="${p%% *}"
        c="$("${cu}" -sk "${@}" --max-time 10 -X "${m}" -o /dev/null -w '%{http_code}' "${u}${p}")"
        [ "${c}" = "${want}" ] || why="${why} ${m} ${p}=${c} (want ${want});"
    done
    if [ -z "${why}" ]; then pass "${label}: unrouted PUT/DELETE/POST 404, POST /ok 405, GET 404/200"
    else fail "${label}:${why}"; fi
}

start plain
if [ -z "${PORT}" ]; then fail "plain server did not start: $(head -2 err.log | tr '\n' ' ')"
else method_cases "HTTP/1.1" curl "http://127.0.0.1:${PORT}" --http1.1; fi
stop "${SRV}"; SRV=""

if [ "${HAVE_TLS}" = 1 ]; then
    start tls
    if [ -z "${PORT}" ]; then fail "TLS server did not start: $(head -2 err.log | tr '\n' ' ')"
    else
        gzip_cases "HTTP/1.1+TLS" curl "https://127.0.0.1:${PORT}" 1.1 --http1.1
        method_cases "HTTP/1.1+TLS" curl "https://127.0.0.1:${PORT}" --http1.1
        if [ -n "${H2CURL}" ]; then
            gzip_cases "HTTP/2" "${H2CURL}" "https://127.0.0.1:${PORT}" 2 --http2
            method_cases "HTTP/2" "${H2CURL}" "https://127.0.0.1:${PORT}" --http2
        else echo "  SKIP: HTTP/2 cases -- no curl here can negotiate h2"; fi
    fi
    stop "${SRV}"; SRV=""
else echo "  SKIP: TLS and HTTP/2 cases -- no openssl CLI to make a certificate"; fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
