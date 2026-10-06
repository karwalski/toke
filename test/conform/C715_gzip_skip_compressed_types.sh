#!/usr/bin/env bash
# C715_gzip_skip_compressed_types.sh -- gzip leaves already-compressed content
# types alone and still compresses text, in http.c's server on HTTP/1.1,
# HTTP/1.1 over TLS and HTTP/2, and in std.router's gzip middleware
# (story 121.89).
#
# THE DEFECT, measured on main (eb4bdb5b): maybe_gzip_body (http.c, the one
# gzip step of all three transports) looked at size, status, Accept-Encoding
# and an existing Content-Encoding -- never at the content type.  Once 121.86
# let binary files reach it whole, every image, font and wasm module over 256
# bytes that http.servedir sent to a gzip-accepting client was gzipped: CPU
# spent for a body as large as before, or larger.  std.router's middleware had
# its own shorter list (image/, video/, audio/, an exact
# application/octet-stream): it recompressed fonts, wasm and archives, and
# skipped image/svg+xml -- which is XML text and compresses well.
#
# THE FIX.  One list, src/stdlib/tk_gzip_type.h, used by both: image/ (but not
# image/svg+xml), video/, audio/, font/, application/octet-stream,
# application/wasm, application/zip, application/gzip (and x-gzip), matched
# without regard to case or parameters.
#
# WHAT IS ASSERTED.
#   http.servedir, with Accept-Encoding: gzip (curl --compressed), on each of
#   HTTP/1.1, TLS and h2: img.png (a ~10 KiB noisy PNG), font.woff2 and
#   app.wasm arrive with no Content-Encoding and byte-identical; page.html and
#   pic.svg arrive with Content-Encoding: gzip and decode byte-identical.
#   std.router (C harness under ASan/UBSan, router_static + router_use_gzip +
#   router_dispatch_ex): the same five files -- png, woff2, wasm not encoded;
#   html and svg encoded.
#
# Negative control: C715_STDLIB=<dir> builds both against another stdlib
# tree.  Against main's src/stdlib (eb4bdb5b): 10 passed, 10 failed -- png,
# woff2 and wasm gzipped on all three transports, svg not gzipped by the
# router (main's router truncated woff2 and wasm at their first 0x00, under
# its 256-byte minimum, so they pass there by accident).  Against the tree
# with 121.88 alone: 8 passed, 12 failed -- the router gzips woff2 and wasm.
#
# Story: 121.89
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C715_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found (fixtures)"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c715_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C715: gzip skips already-compressed types, still compresses text (121.89)"
echo "-------------------------------------------------------------------------"
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

mkdir -p pub/s
python3 - <<'PY2'
import random, struct, zlib
random.seed(121089)
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
w = h = 64
raw = b"".join(b"\x00" + bytes(random.randrange(256) for _ in range(w * 3)) for _ in range(h))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
open("pub/s/img.png", "wb").write(png)
open("pub/s/font.woff2", "wb").write(b"wOF2" + bytes(random.randrange(256) for _ in range(2044)))
open("pub/s/app.wasm", "wb").write(b"\x00asm\x01\x00\x00\x00" + bytes(random.randrange(256) for _ in range(2040)))
open("pub/s/page.html", "w").write("<!DOCTYPE html><html><body>" + "<p>compress me, I am text</p>\n" * 100 + "</body></html>\n")
open("pub/s/pic.svg", "w").write('<svg xmlns="http://www.w3.org/2000/svg">' + '<rect x="1" y="1" width="9" height="9"/>\n' * 60 + "</svg>\n")
PY2

HAVE_TLS=0
if command -v openssl >/dev/null 2>&1 && openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem \
        -days 2 -nodes -subj "/CN=localhost" > cert.log 2>&1; then HAVE_TLS=1; fi

cat > srv.tk <<TOKE
m=srv;
i=http:std.http;
i=args:std.args;
f=main():i64{
  http.servedir("/s";"${WORK}/pub");
  let r=mut.0;
  if(args.get(1)=="tls"){ r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";1) }el{
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

hdr() {   # hdr FILE NAME -> value of header NAME (lower-cased name match)
    tr -d '\r' < "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//'
}


# check LABEL CURL BASE FILE WANT(gzip|none) [CURL-OPTS...]
check() {
    local label="$1" cu="$2" u="$3" f="$4" want="$5"; shift 5
    local r ce
    r="$("${cu}" -sk "$@" --compressed --max-time 10 -o got.bin -D got.hdr -w '%{http_code}' "${u}/s/${f}")"
    ce="$(hdr got.hdr content-encoding)"
    if [ "${r}" = 200 ] && cmp -s got.bin "pub/s/${f}" \
        && { { [ "${want}" = gzip ] && [ "${ce}" = gzip ]; } || { [ "${want}" = none ] && [ -z "${ce}" ]; }; }; then
        pass "${label}: ${f} content-encoding '${ce:-none}', whole"
    else
        fail "${label}: ${f}: ${r} content-encoding '${ce:-none}' (want ${want})$(cmp -s got.bin "pub/s/${f}" || echo ', bytes differ')"
    fi
}
cases() {
    local label="$1" cu="$2" u="$3"; shift 3
    check "${label}" "${cu}" "${u}" img.png    none "$@"
    check "${label}" "${cu}" "${u}" font.woff2 none "$@"
    check "${label}" "${cu}" "${u}" app.wasm   none "$@"
    check "${label}" "${cu}" "${u}" page.html  gzip "$@"
    check "${label}" "${cu}" "${u}" pic.svg    gzip "$@"
}

start plain
if [ -z "${PORT}" ]; then fail "plain server did not start: $(head -2 err.log | tr '\n' ' ')"
else cases "HTTP/1.1" curl "http://127.0.0.1:${PORT}" --http1.1; fi
stop "${SRV}"; SRV=""

if [ "${HAVE_TLS}" = 1 ]; then
    start tls
    if [ -z "${PORT}" ]; then fail "TLS server did not start: $(head -2 err.log | tr '\n' ' ')"
    else
        cases "HTTP/1.1+TLS" curl "https://127.0.0.1:${PORT}" --http1.1
        if [ -n "${H2CURL}" ]; then cases "HTTP/2" "${H2CURL}" "https://127.0.0.1:${PORT}" --http2
        else echo "  SKIP: HTTP/2 cases -- no curl here can negotiate h2"; fi
    fi
    stop "${SRV}"; SRV=""
else echo "  SKIP: TLS and HTTP/2 cases -- no openssl CLI to make a certificate"; fi

# ── std.router's gzip middleware ─────────────────────────────────────────
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
if ! LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects router -- "${CF[@]}")"; then
    fail "stdlib link set: see stderr above"
else
    LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
    cat > r.c <<'EOF'
#include "router.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
static int bad;
static void want(TkRouter *r, const char *url, int gz)
{
    const char *hn[] = { "Accept-Encoding" }, *hv[] = { "gzip" };
    TkRouteResp p = router_dispatch_ex(r, "GET", url, NULL, NULL, hn, hv, 1);
    int enc = 0;
    for (uint64_t i = 0; i < p.nheaders; i++)
        if (p.header_names[i] && strcasecmp(p.header_names[i], "Content-Encoding") == 0) enc = 1;
    int ok = p.status == 200 && enc == gz;
    printf("%s router %s (%s): %s (want %s)\n", ok ? "OK" : "BAD", url,
           p.content_type ? p.content_type : "?", enc ? "gzip" : "not encoded",
           gz ? "gzip" : "not encoded");
    if (!ok) bad++;
}
int main(int argc, char **argv)
{
    (void)argc;
    TkRouter *r = router_new();
    router_static(r, "/s", argv[1]);
    router_use_gzip(r, 256);
    want(r, "/s/img.png", 0);
    want(r, "/s/font.woff2", 0);
    want(r, "/s/app.wasm", 0);
    want(r, "/s/page.html", 1);
    want(r, "/s/pic.svg", 1);
    return bad ? 1 : 0;
}
EOF
    if ! ASAN_OPTIONS=detect_leaks=0 "${CC}" "${CF[@]}" -o r r.c "${LINK[@]}" -lz > r.cc 2>&1; then
        fail "router harness build: $(grep -a -m2 error r.cc | head -c 400)"
    else
        ASAN_OPTIONS=detect_leaks=0 ./r "${WORK}/pub/s" > r.out 2>&1
        while IFS= read -r line; do
            case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
        done < <(grep -a -E '^(OK|BAD) ' r.out)
        grep -a -q -E '^(OK|BAD) ' r.out || fail "router harness produced nothing: $(head -c 300 r.out)"
        if grep -a -q -E 'Sanitizer|runtime error' r.out; then
            fail "sanitizer report: $(grep -a -m1 -E 'Sanitizer|runtime error' r.out | head -c 300)"
        fi
    fi
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
