#!/usr/bin/env bash
# C697_http2_negotiated_over_alpn.sh -- the TLS servers offer h2 in ALPN, and
# large responses are served whole over the negotiated HTTP/2 connection
# (story 121.80).
#
# THE DEFECT, measured on main (13f1d3ae): http.c's alpn_protos offered only
# http/1.1 -- h2 was taken out on 2026-05-16 with "h2 has bugs with large
# responses (CSS 24KB causes worker hang)" -- so no client could reach the
# HTTP/2 server at all, and every HTTP/2 fix since (121.44, 121.46, 121.50,
# 121.62) ran only in C harnesses that installed their own ALPN callback.
#
# THE LARGE-RESPONSE BUG IS GONE.  With h2 put back in ALPN, curl --http2
# fetched 24 KiB, 64 KiB, 70000-byte, 1 MiB and 8 MiB bodies byte-identical,
# one at a time and four multiplexed on one connection, on one and on two
# workers.  The same run against the 2026-05 dispatch (the stdlib of
# f605aec9, before 121.50) also passed with curl, because curl opens large
# flow-control windows; that code wrote DATA ignoring the peer's windows,
# which overruns a client on the default 64 KiB window (C600 shows it).
#
# WHAT DID STILL HOLD A WORKER: an idle HTTP/2 connection was closed after
# 30 s, against 2 s for an idle HTTP/1.1 keep-alive connection.  A pre-fork
# worker serves one connection at a time, so with h2 offered one browser's
# idle connection kept a worker from every other client for 30 s (measured:
# a second client on a one-worker server waited 30.7 s; 1.7 s over HTTP/1.1).
# H2_IDLE_TIMEOUT_S now defaults to the keep-alive clock.
#
# WHAT IS ASSERTED, against a real tkc-built http.servetlsworkerson server:
#   - curl --http2 negotiates HTTP/2 (%{http_version} = 2);
#   - http.servedir files of 24 KiB, 64 KiB, 70000 B, 1 MiB and 8 MiB come back
#     whole and byte-identical over HTTP/2, on 1 and on 2 workers;
#   - the four large ones multiplexed on one connection all complete;
#   - HEAD over HTTP/2 carries the file's content-length;
#   - an idle h2 connection (preface, SETTINGS, one request, then nothing) on
#     a one-worker server does not keep a second client waiting past 5 s.
# The curl cases need a curl that can negotiate h2 (OpenSSL + nghttp2; macOS's
# SecureTransport build cannot); the idle case needs python3's ssl module.
# Each prints a named SKIP when its tool is missing.
#
# Negative control: C697_STDLIB=<dir> builds the server against another
# stdlib tree.  Against main's src/stdlib every curl case fails (HTTP/1.1
# negotiated) and the idle case fails (h2 not negotiated).
#
# Story: 121.80, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C697_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "CONFORM-SKIP: no openssl CLI to make a certificate"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c697_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C697: HTTP/2 is negotiated over ALPN and serves large responses whole (121.80)"
echo "-------------------------------------------------------------------------------"
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if ! openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 2 \
        -nodes -subj "/CN=localhost" > cert.log 2>&1; then
    echo "CONFORM-SKIP: openssl could not make a certificate"; exit 0
fi
mkdir -p pub/s
FILES="c24k.css:24576 b64k.txt:65536 b70k.txt:70000 b1m.txt:1048576 b8m.txt:8388608"
for fs in ${FILES}; do
    # printable bytes: a Res body is a C string
    LC_ALL=C awk -v n="${fs#*:}" 'BEGIN{ s="abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\n";
        while (length(s) < 65536) s = s s; for (k = n; k > 0; k -= length(s)) printf "%s", substr(s, 1, (k < length(s)) ? k : length(s)) }' \
        > "pub/s/${fs%%:*}"
done

cat > srv.tk <<TOKE
m=srv;
i=http:std.http;
i=args:std.args;
f=main():i64{
  http.getstatic("/ok"; "ok");
  http.servedir("/s";"${WORK}/pub");
  let r=mut.0;
  if((mt args.get(1){\$ok:v v;\$err:e ""})=="2"){ r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";2) }el{
  r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";1) };
  <0
};
TOKE
if ! "${TKC}" --allow-all --out srv_bin srv.tk > build.log 2>&1; then
    echo "  FAIL: the server does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

start() {   # start WORKERS -> sets PORT, SRV
    ./srv_bin "$1" > out.log 2> err.log &
    SRV=$!; PORT=""
    tk_wait_until "${SRV}" tk_listening err.log
    PORT="$(tk_listening_port err.log)"
}

# A curl that can negotiate h2: it must list the HTTP2 feature and be built
# on OpenSSL (or a fork), which does ALPN; SecureTransport builds do not.
H2CURL=""
for c in curl /opt/homebrew/opt/curl/bin/curl /usr/local/opt/curl/bin/curl; do
    command -v "${c}" >/dev/null 2>&1 || continue
    v="$("${c}" -V 2>/dev/null)"
    case "${v}" in *SecureTransport*) continue ;; esac
    case "${v}" in *OpenSSL*|*LibreSSL*|*BoringSSL*|*quictls*) ;; *) continue ;; esac
    if printf '%s\n' "${v}" | grep -q '^Features:.*HTTP2'; then H2CURL="${c}"; break; fi
done

curl_cases() {   # curl_cases WORKERS
    local w="$1" why="" f n r
    start "${w}"
    if [ -z "${PORT}" ]; then fail "${w} worker(s): server did not start"; stop "${SRV}"; SRV=""; return; fi
    local u="https://127.0.0.1:${PORT}"
    r="$("${H2CURL}" -sk --http2 --max-time 10 -o ok.body -w '%{http_version} %{http_code}' "${u}/ok")"
    if [ "${r}" = "2 200" ] && [ "$(cat ok.body)" = ok ]; then
        pass "${w} worker(s): curl --http2 negotiates HTTP/2"
    else
        fail "${w} worker(s): curl --http2 got '${r}' (want '2 200')"
    fi
    for fs in ${FILES}; do
        f="${fs%%:*}"; n="${fs#*:}"
        r="$("${H2CURL}" -sk --http2 --max-time 30 -o got.bin -w '%{http_version} %{http_code} %{size_download}' "${u}/s/${f}")"
        if [ "${r}" = "2 200 ${n}" ] && cmp -s got.bin "pub/s/${f}"; then :
        else why="${why} ${f}: '${r}'$(cmp -s got.bin "pub/s/${f}" || echo ' bytes differ');"; fi
    done
    if [ -z "${why}" ]; then pass "${w} worker(s): 24 KiB, 64 KiB, 70000 B, 1 MiB and 8 MiB whole over HTTP/2"
    else fail "${w} worker(s):${why}"; fi
    r="$("${H2CURL}" -sk --http2 --parallel --parallel-max 8 --max-time 60 \
        -w '%{http_version} %{http_code} %{size_download}\n' \
        -o p1 -o p2 -o p3 -o p4 \
        "${u}/s/b8m.txt" "${u}/s/b1m.txt" "${u}/s/b70k.txt" "${u}/s/b64k.txt" | sort | tr '\n' ' ')"
    if [ "${r}" = "2 200 1048576 2 200 65536 2 200 70000 2 200 8388608 " ] \
        && cmp -s p1 pub/s/b8m.txt && cmp -s p2 pub/s/b1m.txt; then
        pass "${w} worker(s): four large responses multiplexed on one connection"
    else fail "${w} worker(s): multiplexed: ${r}"; fi
    r="$("${H2CURL}" -sk --http2 -I --max-time 10 "${u}/s/b1m.txt" | tr -d '\r' \
        | grep -iE '^(HTTP/|content-length:)' | sed 's/ *$//' | tr '\n' ' ')"
    if [ "${r}" = "HTTP/2 200 content-length: 1048576 " ]; then pass "${w} worker(s): HEAD over HTTP/2 carries the length"
    else fail "${w} worker(s): HEAD: '${r}'"; fi
    stop "${SRV}"; SRV=""
}

if [ -n "${H2CURL}" ]; then
    curl_cases 1
    curl_cases 2
else
    echo "  SKIP: the curl cases -- no curl here can negotiate h2 (one built with OpenSSL and nghttp2 is needed)"
fi

# An idle HTTP/2 connection must not keep a one-worker server from a second
# client for longer than an idle HTTP/1.1 connection would.
if command -v python3 >/dev/null 2>&1 && python3 -c 'import ssl' 2>/dev/null; then
    cat > idle.py <<'PY'
import socket, ssl, sys, time
port = int(sys.argv[1])
def conn(protos, t=20):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
    ctx.set_alpn_protocols(protos)
    return ctx.wrap_socket(socket.create_connection(("127.0.0.1", port), timeout=t))
a = conn(["h2", "http/1.1"])
if a.selected_alpn_protocol() != "h2":
    print("not negotiated: %s" % a.selected_alpn_protocol()); sys.exit(1)
blk = bytes([0x82, 0x87, 0x44, 3]) + b"/ok" + bytes([0x41, 1]) + b"t"
a.sendall(b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" + bytes(9)[:3] + bytes([4]) + bytes(5)
          + bytes([0, 0, len(blk), 1, 5, 0, 0, 0, 1]) + blk)
time.sleep(0.3)                       # served; the connection now idles
t0 = time.time()
got = b""
try:
    b = conn(["http/1.1"], 8)
    b.settimeout(8)
    b.sendall(b"GET /ok HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
    while True:
        d = b.recv(4096)
        if not d: break
        got += d
except OSError as e:
    got = got or ("(%s)" % e.__class__.__name__).encode()
took = time.time() - t0
ok = got.startswith(b"HTTP/1.1 200") and took < 5.0
print("%s second client: %r after %.1f s" % ("ok" if ok else "held", got[:12], took))
sys.exit(0 if ok else 1)
PY
    start 1
    if [ -z "${PORT}" ]; then fail "idle: server did not start"
    elif out="$(python3 idle.py "${PORT}" 2>&1)"; then
        pass "an idle h2 connection does not hold the only worker (${out})"
    else fail "idle h2 connection: ${out}"; fi
    stop "${SRV}"; SRV=""
else
    echo "  SKIP: the idle case -- no python3 with ssl"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
