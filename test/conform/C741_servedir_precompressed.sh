#!/usr/bin/env bash
# C741_servedir_precompressed.sh -- http.servedir serves a file's
# precompressed variant (file.br, file.gz, file.zst) when the request's
# Accept-Encoding admits its coding, with Content-Encoding and Vary, and
# never one that resolves outside the mount (story 121.99).
#
# THE GAP, measured on main (b442a5d7) with this script.  content.c's
# content_find_precompressed (fixed under 121.96, C731) had no caller:
# http.servedir read the file itself on every request and the writer gzipped
# it on the fly, so a site that ships app.css.br and app.css.gz beside
# app.css (the usual build output) never had them served -- a client that
# accepts br got gzip made per request, and one that accepts only br got the
# file whole.  Nor did a servedir response say "Vary: Accept-Encoding"
# unless the writer happened to gzip it, so a cache could hand a gzip-less
# client a body chosen for another.  (content_select_encoding and
# content_negotiate_language, also uncalled and carrying a second q-value
# parser, are retired by the same story.)
#
# THE FIX.  staticdir_handler asks content_find_precompressed for the
# variant of the resolved file that the request accepts, br before gzip
# before zstd; it resolves that variant under the mount root exactly as it
# resolves the file (realpath, 121.18), and serves it with the file's
# Content-Type, its Content-Encoding and "Vary: Accept-Encoding".  The
# writer does not gzip it again (a Content-Encoding is set).  A file with
# any variant says Vary even when served as it is; the writer's own gzip
# then adds no second Vary.  The fs.read gate (121.42, C470) is unchanged:
# it is taken at http.servedir.
#
# WHAT IS ASSERTED, over a real socket against a toke server, plain and TLS
# (raw bytes), and over HTTP/2 where curl here speaks it:
#   1. app.css with "gzip": app.css.gz's bytes, Content-Encoding gzip,
#      one Vary: Accept-Encoding, Content-Type text/css, its length;
#   2. with "br, gzip": app.css.br's bytes, Content-Encoding br;
#   3. with "gzip;q=0", with none, with "zstd" (no .zst on disk): the file
#      as it is, no Content-Encoding, Vary: Accept-Encoding;
#   4. only.js (only only.js.br on disk) with "gzip": gzipped on the fly by
#      the writer -- inflates to the file -- with exactly one Vary line;
#   5. plain.txt (no variants) with "gzip": no Vary, no Content-Encoding;
#   6. evil.css whose evil.css.gz is a symlink out of the mount: the file as
#      it is, never the outside bytes;
#   7. the directory /static/ with index.html.gz: the variant;
#   8. HEAD app.css with "gzip": the GET's framing fields (the variant's
#      length, or chunked as the GET is) and coding, no body;
#   9. /static/%2e%2e/outside/secret.gz: refused (400/403/404), as before.
#
# Negative control: C741_STDLIB=<dir> builds the server against another
# stdlib tree (TKC_STDLIB_DIR).  Against a git archive of main's src/stdlib
# and stdlib (b442a5d7): 29 passed, 15 failed -- no variant is ever served
# (gzip made per request instead of app.css.gz, gzip for a br client, the
# index gzipped on the fly), no Vary on a file that has variants, and HEAD
# says the uncompressed length (121.100); plain, TLS and h2.  What passes is
# no-variant files, the symlink and traversal refusals, and the writer's own
# gzip.  On this branch: 44 passed (42 without an h2-capable curl).
#
# Story: 121.99, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C741_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c741_XXXXXX")"
WORK="$(cd "${WORK}" && pwd -P)"
cd "${WORK}" || exit 1
echo "C741: http.servedir serves precompressed variants (121.99)"
echo "----------------------------------------------------------------------"

# The tree: public/static/... is served under /static; outside/ is not.
python3 - <<'PY'
import gzip, os
os.makedirs("public/static", exist_ok=True)
os.makedirs("outside", exist_ok=True)
def w(p, b):
    with open(p, "wb") as f:
        f.write(b)
css = b"body{color:#123456;margin:0}\n" * 40
w("public/static/app.css", css)
w("public/static/app.css.gz", gzip.compress(css, mtime=0))
w("public/static/app.css.br", b"BROTLI-VARIANT-BYTES\x00\x01")
js = b"function f(){return 42}\n" * 40
w("public/static/only.js", js)
w("public/static/only.js.br", b"ONLY-BR")
w("public/static/plain.txt", b"plain text\n" * 40)
w("public/static/evil.css", b"evil{}\n")
w("outside/secret.gz", b"OUTSIDE-SECRET")
os.symlink(os.path.abspath("outside/secret.gz"), "public/static/evil.css.gz")
html = b"<!doctype html><title>i</title>\n" * 20
w("public/static/index.html", html)
w("public/static/index.html.gz", gzip.compress(html, mtime=0))
PY

build() {
    printf 'm=%s;\ni=http:std.http;\nf=main():i64{\n  http.servedir("/static"; "%s/public");\n  %s;\n  <0\n};\n' \
        "$1" "${WORK}" "$2" > "$1.tk"
    "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1
}
start() {
    ./"$1" >/dev/null 2>"$1.err" & SRV=$!
    PORT=""
    tk_wait_until "${SRV}" tk_listening "$1.err"
    PORT="$(tk_listening_port "$1.err")"
}
trap 'stop; rm -rf "${WORK}"' EXIT
stop() { [ -n "${SRV}" ] && { pkill -P "${SRV}" 2>/dev/null; kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; SRV=""; }
collect() {
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "$1${line#OK }" ;; BAD\ *) fail "$1${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' cli.out)
    grep -a -q -E '^(OK|BAD) ' cli.out || fail "$1the client produced nothing: $(head -c 300 cli.out)"
    grep -a -q -E '^Traceback' cli.out && fail "$1the client failed: $(tail -2 cli.out | head -c 300)"
}

cat > cli.py <<'PY'
import socket, ssl, sys, zlib
port = int(sys.argv[1])
tls = len(sys.argv) > 2 and sys.argv[2] == "tls"
S = "public/static/"
def rd(p):
    with open(p, "rb") as f:
        return f.read()
def raw(req):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s, server_hostname="localhost")
    s.sendall(req)
    out = b""
    while True:
        try:
            b = s.recv(65536)
        except (socket.timeout, ssl.SSLError):
            break
        if not b:
            break
        out += b
    s.close()
    return out
def fields(head):
    d = {}
    for l in head.split(b"\r\n")[1:]:
        if b":" in l:
            k, _, v = l.partition(b":")
            d.setdefault(k.strip().lower(), []).append(v.strip())
    return d
def dechunk(b):
    out = b""
    while b:
        n, _, b = b.partition(b"\r\n")
        try:
            n = int(n, 16)
        except ValueError:
            return None
        if n == 0:
            return out
        out += b[:n]
        b = b[n + 2:]
    return None
def fetch(path, ae, method=b"GET"):
    r = b"%s %s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n" % (method, path)
    if ae is not None:
        r += b"Accept-Encoding: " + ae + b"\r\n"
    out = raw(r + b"\r\n")
    h, _, body = out.partition(b"\r\n\r\n")
    f = fields(h)
    if method != b"HEAD" and f.get(b"transfer-encoding") == [b"chunked"]:
        body = dechunk(body)
    return h, f, body
def report(ok, msg):
    print(("OK " if ok else "BAD ") + msg)
VARY = [b"Accept-Encoding"]

# 1. gzip variant
h, f, b = fetch(b"/static/app.css", b"gzip")
report(h.startswith(b"HTTP/1.1 200"), "app.css gzip: 200 (%r)" % h[:20])
report(b == rd(S + "app.css.gz"), "app.css gzip: the bytes of app.css.gz (%d bytes, want %d)" % (len(b or b""), len(rd(S + "app.css.gz"))))
report(f.get(b"content-encoding") == [b"gzip"], "app.css gzip: Content-Encoding gzip (%r)" % f.get(b"content-encoding"))
report(f.get(b"vary") == VARY, "app.css gzip: one Vary: Accept-Encoding (%r)" % f.get(b"vary"))
report((f.get(b"content-type") or [b""])[0].startswith(b"text/css"), "app.css gzip: the file's Content-Type (%r)" % f.get(b"content-type"))
# 2. br first
h, f, b = fetch(b"/static/app.css", b"br, gzip")
report(b == rd(S + "app.css.br") and f.get(b"content-encoding") == [b"br"],
       "app.css br, gzip: app.css.br, Content-Encoding br (%r %r)" % (f.get(b"content-encoding"), (b or b"")[:20]))
report(f.get(b"vary") == VARY, "app.css br: one Vary (%r)" % f.get(b"vary"))
# 3. the file as it is, but it varies
for ae in (b"gzip;q=0", None, b"zstd"):
    h, f, b = fetch(b"/static/app.css", ae)
    tag = "app.css %s" % (ae.decode() if ae else "(no Accept-Encoding)")
    report(b == rd(S + "app.css") and f.get(b"content-encoding") is None,
           "%s: the file as it is (%r)" % (tag, f.get(b"content-encoding")))
    report(f.get(b"vary") == VARY, "%s: Vary: Accept-Encoding (%r)" % (tag, f.get(b"vary")))
# 4. only a .br; the writer gzips; one Vary
h, f, b = fetch(b"/static/only.js", b"gzip")
try:
    inflated = zlib.decompress(b, 16 + zlib.MAX_WBITS)
except Exception:
    inflated = None
report(f.get(b"content-encoding") == [b"gzip"] and inflated == rd(S + "only.js"),
       "only.js gzip: gzipped by the writer, inflates to the file (%r)" % f.get(b"content-encoding"))
report(f.get(b"vary") == VARY, "only.js gzip: exactly one Vary: Accept-Encoding (%r)" % f.get(b"vary"))
# 5. no variants: no Vary from servedir
h, f, b = fetch(b"/static/plain.txt", b"identity")
report(b == rd(S + "plain.txt") and f.get(b"vary") is None and f.get(b"content-encoding") is None,
       "plain.txt: no Vary, no Content-Encoding (%r %r)" % (f.get(b"vary"), f.get(b"content-encoding")))
# 6. a variant symlinked out of the mount is not served
h, f, b = fetch(b"/static/evil.css", b"gzip")
report(b"OUTSIDE" not in (b or b"") and b == rd(S + "evil.css") and f.get(b"content-encoding") is None,
       "evil.css: the outside variant is not served; the file is (%r %r)" % (f.get(b"content-encoding"), (b or b"")[:20]))
# 7. a directory's index
h, f, b = fetch(b"/static/", b"gzip")
report(b == rd(S + "index.html.gz") and f.get(b"content-encoding") == [b"gzip"],
       "/static/: index.html.gz (%r)" % f.get(b"content-encoding"))
report((f.get(b"content-type") or [b""])[0].startswith(b"text/html"), "/static/: text/html (%r)" % f.get(b"content-type"))
# 8. HEAD
gh, gf, gb = fetch(b"/static/app.css", b"gzip")
h, f, b = fetch(b"/static/app.css", b"gzip", b"HEAD")
K = (b"content-length", b"transfer-encoding", b"content-encoding", b"vary")
cl = f.get(b"content-length")
report(f.get(b"content-encoding") == [b"gzip"] and b == b"" and
       (cl is None or cl == [str(len(rd(S + "app.css.gz"))).encode()]) and
       all(f.get(k) == gf.get(k) for k in K),
       "HEAD app.css gzip: the GET's framing and coding (the variant's), no body (%r %r %r)"
       % (cl, f.get(b"transfer-encoding"), f.get(b"content-encoding")))
# 9. traversal still refused
h, f, b = fetch(b"/static/%2e%2e/%2e%2e/outside/secret.gz", b"gzip")
report(not h.startswith(b"HTTP/1.1 200") and b"OUTSIDE" not in (b or b""), "traversal: refused (%r)" % h[:20])
PY

if ! build srv 'http.serveon("127.0.0.1"; 0)'; then
    fail "the server does not build: $(grep -m1 '^error' srv.log)"
else
    start srv
    if [ -z "${PORT}" ]; then fail "the server did not start: $(head -3 srv.err)"
    else python3 cli.py "${PORT}" > cli.out 2>&1; collect ""; fi
    stop
fi

H2CURL=""
for c in curl /opt/homebrew/opt/curl/bin/curl /usr/local/opt/curl/bin/curl; do
    command -v "${c}" >/dev/null 2>&1 || continue
    v="$("${c}" -V 2>/dev/null)"
    case "${v}" in *SecureTransport*) continue ;; esac
    case "${v}" in *OpenSSL*|*LibreSSL*|*BoringSSL*|*quictls*) ;; *) continue ;; esac
    if printf '%s\n' "${v}" | grep -q '^Features:.*HTTP2'; then H2CURL="${c}"; break; fi
done
hdr() { tr -d '\r' < "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//'; }

if ! command -v openssl >/dev/null 2>&1; then
    fail "tls: no openssl(1) on this host to make a test cert"
elif ! openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 2 \
        -nodes -subj "/CN=localhost" > cert.log 2>&1 || [ ! -s cert.pem ]; then
    fail "tls: openssl could not make a test cert"
elif ! build srvtls "http.servetlson(\"127.0.0.1\"; 0; \"${WORK}/cert.pem\"; \"${WORK}/key.pem\")"; then
    fail "tls: the server does not build: $(grep -m1 '^error' srvtls.log)"
else
    start srvtls
    if [ -z "${PORT}" ]; then fail "tls: the server did not start: $(head -3 srvtls.err)"
    else
        python3 cli.py "${PORT}" tls > cli.out 2>&1; collect "tls: "
        if [ -n "${H2CURL}" ]; then
            "${H2CURL}" -sk --http2 -H 'Accept-Encoding: br, gzip' -D h2.h -o h2.b \
                "https://127.0.0.1:${PORT}/static/app.css" > /dev/null 2>&1
            v="$(head -1 h2.h | tr -d '\r')"
            if [ "${v%% *}" = "HTTP/2" ] && [ "$(hdr h2.h content-encoding)" = "br" ] &&
               cmp -s h2.b public/static/app.css.br; then
                pass "h2: app.css with br, gzip is app.css.br, content-encoding br"
            else fail "h2: app.css with br, gzip: ${v}; content-encoding '$(hdr h2.h content-encoding)'"; fi
            n="$(tr -d '\r' < h2.h | grep -ci '^vary:')"
            if [ "${n}" = "1" ] && [ "$(hdr h2.h vary)" = "Accept-Encoding" ]; then
                pass "h2: one vary: Accept-Encoding"
            else fail "h2: ${n} vary fields ($(hdr h2.h vary))"; fi
        else
            echo "  SKIP: HTTP/2 cases -- no curl here can negotiate h2"
        fi
    fi
    stop
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
