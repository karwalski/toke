#!/usr/bin/env bash
# S004_static_query_string.sh — a versioned asset URL must resolve (127.100).
#
# `/static/css/style.css?v=<hash>` returned 404 while the same path without
# the query returned 200.  Serving an asset under a versioned URL is the
# standard way to make a cache-bust reliable, so this made cache-busting
# unavailable: a stylesheet change had to wait out the CDN.  It was found by
# deploying such a URL to the live site and leaving it unstyled.
#
# The cause was that nothing in the server ever separated the path from the
# query.  A request target is `absolute-path [ "?" query ]` (RFC 9110 4.1);
# route matching and file resolution both saw the raw target, so
#   * http.getstatic("/api/version") never matched "/api/version?x=1";
#   * http.servedir's handler built the filename "build/style.css?v=abc",
#     which no filesystem holds.
#
# This drives a real server over a real socket — a fix that merely compiles
# proves nothing — and asserts 200 WITH THE RIGHT BYTES for every static
# entry point: servedir, servepages, getstatic and getstaticmime.  A
# fragment is never sent on the wire, but is stripped on the same principle.
#
# Story: 127.100

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_qstring_XXXXXX)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        pkill -9 -P "${SRV_PID}" 2>/dev/null
        kill -9 "${SRV_PID}" 2>/dev/null
    fi
    rm -rf "${WORK}"
}
trap cleanup EXIT
cd "${WORK}"

echo "S004: a query string must not stop a static asset from resolving"
echo "--------------------------------------"

# Everything here is served over loopback on an ephemeral port.  No CDN is
# involved: requesting a URL through one before it exists caches the 404.
PORT=0
for _try in $(seq 1 10); do
    CAND=$(( 20000 + RANDOM % 20000 ))
    if ! (exec 3<>"/dev/tcp/127.0.0.1/${CAND}") 2>/dev/null; then
        PORT="${CAND}"
        break
    fi
done
if [ "${PORT}" -eq 0 ]; then
    echo "  FAIL: could not find a free port"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

CSS_BODY='body{color:#e2b714}'
STATIC_BODY='getstatic-served-body'
MIME_BODY='mime-served-body'

mkdir -p build/static/css templates
printf '%s' "${CSS_BODY}" > build/static/css/style.css

cat > templates/base.tkt <<'TPL'
<!DOCTYPE html><html><head><title>{! yield("title") !}</title></head>
<body>{! yield("content") !}</body></html>
TPL
cat > templates/page.tkt <<'TPL'
{! layout("base") !}
{! block("title") !}T{! end !}
{! block("content") !}PAGE-BODY-OK{! end !}
TPL

cat > srv.tk <<TOKE
m=qstringsrv;
i=http:std.http;

f=main():i64{
  http.getstatic("/api/version";"${STATIC_BODY}");
  http.getstaticmime("/mime.txt";"${MIME_BODY}";"text/plain; charset=utf-8");
  http.servepages("templates";"templates");
  http.servedir("/";"build");
  http.serveworkers(${PORT};2);
  < 0
};
TOKE

if ! "${TKC}" --allow-all --out srv_bin srv.tk >compile.log 2>&1; then
    echo "  FAIL: server did not compile"
    sed 's/^/      /' compile.log
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

./srv_bin >server.log 2>&1 &
SRV_PID=$!

UP=0
for _i in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then UP=1; break; fi
    sleep 0.1
done
if [ "${UP}" -ne 1 ]; then
    echo "  FAIL: server never accepted a connection on port ${PORT}"
    sed 's/^/      /' server.log
    echo "Results: 0 passed, 1 failed"
    exit 1
fi
echo "  server up on 127.0.0.1:${PORT} (pid ${SRV_PID})"

# check <label> <url-path> <expected-substring>
check() {
    local label="$1" upath="$2" want="$3"
    local out code body
    out="$(curl -sS -m 10 -w '\n%{http_code}' "http://127.0.0.1:${PORT}${upath}" 2>/dev/null)"
    code="${out##*$'\n'}"
    body="${out%$'\n'*}"
    if [ "${code}" != "200" ]; then
        echo "  FAIL: ${label} — GET ${upath} returned ${code}, expected 200"
        FAIL=$((FAIL + 1))
        return
    fi
    case "${body}" in
        *"${want}"*)
            echo "  PASS: ${label} — 200 with the right bytes for ${upath}"
            PASS=$((PASS + 1)) ;;
        *)
            echo "  FAIL: ${label} — GET ${upath} was 200 but served the wrong body"
            echo "        wanted a body containing: ${want}"
            echo "        got: ${body}"
            FAIL=$((FAIL + 1)) ;;
    esac
}

# Baseline: the same paths without a query must work, or the test proves
# nothing about the query.
check "servedir baseline"      "/static/css/style.css"          "${CSS_BODY}"
check "getstatic baseline"     "/api/version"                   "${STATIC_BODY}"
check "getstaticmime baseline" "/mime.txt"                      "${MIME_BODY}"
check "servepages baseline"    "/page"                          "PAGE-BODY-OK"

# The defect: exactly the same assets under a versioned URL.
check "servedir + ?v="         "/static/css/style.css?v=7b2b0cf3f1" "${CSS_BODY}"
check "servedir + multi-param" "/static/css/style.css?v=1&x=2"     "${CSS_BODY}"
check "servedir + empty query" "/static/css/style.css?"            "${CSS_BODY}"
check "getstatic + ?v="        "/api/version?v=abc"                "${STATIC_BODY}"
check "getstaticmime + ?v="    "/mime.txt?v=abc"                   "${MIME_BODY}"
check "servepages + ?from="    "/page?from=nav"                    "PAGE-BODY-OK"

# A fragment never reaches the server from a browser (and curl strips it
# client-side), but a hand-written client can put one on the wire.  Send the
# request target raw so the server really sees the '#'.
raw_get() {
    local target="$1"
    exec 3<>"/dev/tcp/127.0.0.1/${PORT}" || return 1
    printf 'GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' \
        "${target}" >&3
    cat <&3
    exec 3<&-
}
FRAG_RESP="$(raw_get '/static/css/style.css#top' 2>/dev/null)"
case "${FRAG_RESP}" in
    "HTTP/1.1 200"*"${CSS_BODY}"*)
        echo "  PASS: raw on-the-wire fragment — 200 with the right bytes"
        PASS=$((PASS + 1)) ;;
    *)
        echo "  FAIL: raw request for /static/css/style.css#top did not return"
        echo "        200 with the stylesheet; the fragment reached the filename"
        echo "        first line: $(printf '%s' "${FRAG_RESP}" | head -1)"
        FAIL=$((FAIL + 1)) ;;
esac

# The query must not become a path traversal or a way to reach another file.
TRAVERSE_CODE="$(curl -sS -m 10 -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:${PORT}/static/css/nope.css?v=1" 2>/dev/null)"
if [ "${TRAVERSE_CODE}" = "404" ]; then
    echo "  PASS: a missing file is still 404 with a query attached"
    PASS=$((PASS + 1))
else
    echo "  FAIL: missing file with a query returned ${TRAVERSE_CODE}, expected 404"
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
