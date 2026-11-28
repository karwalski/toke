#!/usr/bin/env bash
# C701_http_binary_body.sh -- http.servedir serves a binary file whole: every
# byte, NUL included, with a Content-Length that counts them, over HTTP/1.1,
# HTTP/1.1 on TLS and negotiated HTTP/2, plain and gzip-encoded (story 121.86).
#
# THE DEFECT, measured on main (ce75d43c): staticdir_handler read the file
# into a buffer it then handed on as a C string, and every writer measured the
# body with strlen() -- the chunked and Content-Length writers of the
# cleartext loop, the TLS loop, HEAD on both, the gzip eligibility test, the
# HTTP/2 response (content-length and DATA) and the access log.  So a served
# file was cut at its first 0x00 byte, and its Content-Length said so:
#   - a 16 KiB file of bytes 0x00..0xFF arrived as 0 bytes (200, length 0);
#   - a real 4x4 PNG arrived as its first 8 bytes, its signature: the IHDR
#     length that follows starts with a 0x00;
# on all three transports, and a HEAD reported the truncated length.  Text
# files were never affected: they hold no NUL.
#
# WHY ONLY SERVEDIR.  A handler's body is a toke `str`, and a toke string is a
# bare NUL-terminated buffer with no stored length (spec v0.4, F.1), so no
# handler can return a NUL; servedir is the one server path whose body is
# bytes read from a file.  It now registers the file's length with the
# response (http_body_setlen) and every writer sizes the body with
# http_res_body_len().
#
# WHAT IS ASSERTED, against a real tkc-built server with one worker, for each
# of HTTP/1.1, HTTP/1.1 over TLS and HTTP/2 over TLS:
#   - /s/bytes.bin (0x00..0xFF x 64, 16384 B) -> 200, size_download 16384,
#     byte-identical, Content-Length 16384 (since 121.100 cleartext
#     HTTP/1.1 too: http.servedir sends a fixed length, where it was chunked);
#   - /s/img.png (a generated 4x4 PNG) -> byte-identical, image/png;
#   - HEAD /s/bytes.bin -> Content-Length 16384;
#   - with --compressed (Accept-Encoding: gzip) /s/bytes.bin decodes to the
#     same 16384 bytes.
# The TLS cases need the openssl CLI, the h2 cases a curl that can negotiate
# h2 (OpenSSL + nghttp2), the PNG python3; each prints a named SKIP without.
#
# Negative control: C701_STDLIB=<dir> builds the server against another
# stdlib tree.  Against main's src/stdlib every byte-comparison and length
# case fails (0 bytes / 8 bytes served).
#
# Story: 121.86, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C701_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c701_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C701: http.servedir serves binary files whole on HTTP/1.1, TLS and h2 (121.86)"
echo "-------------------------------------------------------------------------------"
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

mkdir -p pub/s
# 0x00..0xFF, 64 times over: 16384 bytes, the first of them a NUL.
LC_ALL=C awk 'BEGIN{ for (r = 0; r < 64; r++) for (b = 0; b < 256; b++) printf "%c", b }' > pub/s/bytes.bin
BINLEN="$(wc -c < pub/s/bytes.bin | tr -d ' ')"
if [ "${BINLEN}" != 16384 ]; then
    # some awks cannot printf "%c" 0; build it from octal escapes instead
    : > pub/s/bytes.bin
    for r in $(seq 1 64); do
        for b in $(seq 0 255); do printf "\\$(printf '%03o' "${b}")"; done
    done >> pub/s/bytes.bin
    BINLEN="$(wc -c < pub/s/bytes.bin | tr -d ' ')"
fi
[ "${BINLEN}" = 16384 ] || { echo "  FAIL: could not build the 16384-byte fixture (${BINLEN})"; echo "Results: 0 passed, 1 failed"; exit 1; }
HAVE_PNG=0
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'PY' && HAVE_PNG=1
import struct, zlib
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
raw = b"".join(b"\x00" + b"".join(bytes([x * 60, y * 60, 0]) for x in range(4)) for y in range(4))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
open("pub/s/img.png", "wb").write(png)
PY
fi
PNGLEN=0; [ "${HAVE_PNG}" = 1 ] && PNGLEN="$(wc -c < pub/s/img.png | tr -d ' ')"

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
    tk_wait_until "${SRV}" tk_listening err.log
    PORT="$(tk_listening_port err.log)"
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

# cases LABEL CURL URL-BASE WANT-VERSION [CURL-OPTS...]
cases() {
    local label="$1" cu="$2" u="$3" wantv="$4"; shift 4
    local r cl
    r="$("${cu}" -sk "$@" --max-time 10 -o got.bin -D got.hdr -w '%{http_version} %{http_code} %{size_download}' "${u}/s/bytes.bin")"
    cl="$(hdr got.hdr content-length)"
    if [ "${r}" = "${wantv} 200 16384" ] && cmp -s got.bin pub/s/bytes.bin \
        && [ "${cl}" = 16384 ]; then
        pass "${label}: bytes.bin (0x00..0xFF x 64) whole${cl:+, content-length ${cl}}"
    else fail "${label}: bytes.bin: '${r}' content-length '${cl}'$(cmp -s got.bin pub/s/bytes.bin || echo ', bytes differ') (want '${wantv} 200 16384')"; fi
    if [ "${HAVE_PNG}" = 1 ]; then
        r="$("${cu}" -sk "$@" --max-time 10 -o got.png -D got.hdr -w '%{http_code} %{size_download}' "${u}/s/img.png")"
        if [ "${r}" = "200 ${PNGLEN}" ] && cmp -s got.png pub/s/img.png \
            && hdr got.hdr content-type | grep -q '^image/png'; then
            pass "${label}: img.png (${PNGLEN} B) whole, image/png"
        else fail "${label}: img.png: '${r}' (want '200 ${PNGLEN}')"; fi
    else echo "  SKIP: ${label}: img.png -- no python3 to generate it"; fi
    "${cu}" -sk "$@" --max-time 10 -I -o /dev/null -D got.hdr "${u}/s/bytes.bin"
    cl="$(hdr got.hdr content-length)"
    if [ "${cl}" = 16384 ]; then pass "${label}: HEAD bytes.bin content-length 16384"
    else fail "${label}: HEAD bytes.bin content-length '${cl}' (want 16384)"; fi
    r="$("${cu}" -sk "$@" --compressed --max-time 10 -o gz.bin -D got.hdr -w '%{http_code} %{size_download}' "${u}/s/bytes.bin")"
    if [ "${r%% *}" = 200 ] && cmp -s gz.bin pub/s/bytes.bin; then
        pass "${label}: --compressed bytes.bin decodes whole (content-encoding '$(hdr got.hdr content-encoding)')"
    else fail "${label}: --compressed bytes.bin: '${r}' $(wc -c < gz.bin 2>/dev/null | tr -d ' ') B decoded$(cmp -s gz.bin pub/s/bytes.bin || echo ', bytes differ')"; fi
}

start plain
if [ -z "${PORT}" ]; then fail "plain server did not start: $(head -2 err.log | tr '\n' ' ')"
else cases "HTTP/1.1" curl "http://127.0.0.1:${PORT}" 1.1 --http1.1; fi
stop "${SRV}"; SRV=""

if [ "${HAVE_TLS}" = 1 ]; then
    start tls
    if [ -z "${PORT}" ]; then fail "TLS server did not start: $(head -2 err.log | tr '\n' ' ')"
    else
        cases "HTTP/1.1+TLS" curl "https://127.0.0.1:${PORT}" 1.1 --http1.1
        if [ -n "${H2CURL}" ]; then cases "HTTP/2" "${H2CURL}" "https://127.0.0.1:${PORT}" 2 --http2
        else echo "  SKIP: HTTP/2 cases -- no curl here can negotiate h2"; fi
    fi
    stop "${SRV}"; SRV=""
else echo "  SKIP: TLS and HTTP/2 cases -- no openssl CLI to make a certificate"; fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
