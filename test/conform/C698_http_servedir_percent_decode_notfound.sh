#!/usr/bin/env bash
# C698_http_servedir_percent_decode_notfound.sh -- http.servedir names files by
# the percent-decoded request path, refusing what must not reach the
# filesystem, and a servedir miss is the http.setnotfound body (story 121.81).
#
# THE DEFECTS, measured on main (13f1d3ae):
#   1. staticdir_handler joined the RAW request path onto the root, so a file
#      whose name needs escaping in a URL (`a b.txt`, sent as `/a%20b.txt`)
#      could not be served at all: 404.
#   2. A servedir miss answered the plain 9-byte "Not Found" whatever
#      http.setnotfound said -- and since a mount sits on the "*" route, that
#      was every unrouted path of a server with a mount, not only a missing
#      file under it.
#
# NOW: the mount is still chosen on the raw path (routing never decodes); the
# file is named by the decoded path.  A `%` without two hex digits, `%00` and
# `%2F`/`%2f` are 400; a `..` in the DECODED path is 403, as a literal `..`
# always was.  A miss (missing file, path under no mount) is 404 with the
# setnotfound body and text/html when one is set, plain "Not Found" when not.
#
# WHAT IS ASSERTED (plain HTTP, one worker):
#   /s/a%20b.txt and /s/%61%20b.txt -> 200, the file's bytes; /s/plain.txt and
#   /s/c+d.txt (a `+` is a plain character) -> 200;
#   /s/sub%2Fx.txt, /s/sub%2fx.txt, /s/a%00b.txt, /s/bad%zz, /s/trunc%2 -> 400;
#   /s/%2e%2e/secret.txt and /s/%2E%2E/secret.txt -> 403, secret not served;
#   /s/sub/../secret.txt (literal) -> 403 still;
#   with setnotfound: /s/missing.txt and /elsewhere -> 404, the set body,
#   text/html; /ok (a getstatic route) -> 200;
#   without setnotfound: /s/missing.txt -> 404 "Not Found".
#
# Negative control: C698_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib the two decoded fetches are 404, the five
# malformed escapes are 404 (not 400), %2e%2e is 404 (not 403), and both
# setnotfound misses are "Not Found".
#
# Story: 121.81
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C698_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c698_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C698: http.servedir percent-decodes file paths; a miss is the setnotfound body (121.81)"
echo "--------------------------------------------------------------------------------------"
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

mkdir -p pub/s/sub
printf 'space file\n' > "pub/s/a b.txt"
printf 'plain file\n' > pub/s/plain.txt
printf 'plus file\n'  > "pub/s/c+d.txt"
printf 'sub file\n'   > pub/s/sub/x.txt
printf 'SECRET\n'     > pub/secret.txt

BODY='<h1>custom 404</h1>'
cat > srv.tk <<TOKE
m=srv;
i=http:std.http;
i=args:std.args;
f=main():i64{
  http.getstatic("/ok"; "ok");
  if((mt args.get(1){\$ok:v v;\$err:e ""})=="set"){ http.setnotfound("${BODY}") };
  http.servedir("/s";"${WORK}/pub");
  let r=http.serveworkerson("127.0.0.1";0;1);
  <0
};
TOKE
if ! "${TKC}" --allow-all --out srv_bin srv.tk > build.log 2>&1; then
    echo "  FAIL: the server does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

start() {   # start SET -> sets PORT, SRV
    ./srv_bin "$1" > out.log 2> err.log &
    SRV=$!; PORT=""
    for _ in $(seq 1 80); do
        PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' err.log | head -1)"
        [ -n "${PORT}" ] && break
        kill -0 "${SRV}" 2>/dev/null || break
        sleep 0.1
    done
}

# get PATH -> CODE, BODY (file got.body), CT
get() {
    rm -f got.body got.hdr
    CODE="$(curl -s --path-as-is --max-time 5 -o got.body -D got.hdr -w '%{http_code}' "http://127.0.0.1:${PORT}$1")"
    BODYGOT="$(cat got.body 2>/dev/null)"
    CT="$(tr -d '\r' < got.hdr 2>/dev/null | grep -i '^content-type:' | head -1)"
}

start set
if [ -z "${PORT}" ]; then
    fail "server did not start: $(head -2 err.log | tr '\n' ' ')"
else
    for p in "/s/a%20b.txt" "/s/%61%20b.txt"; do
        get "${p}"
        if [ "${CODE}" = 200 ] && [ "${BODYGOT}" = "space file" ]; then pass "${p} serves 'a b.txt'"
        else fail "${p}: ${CODE} '${BODYGOT}' (want 200 'space file')"; fi
    done
    get /s/plain.txt
    [ "${CODE}" = 200 ] && [ "${BODYGOT}" = "plain file" ] && pass "/s/plain.txt still served" \
        || fail "/s/plain.txt: ${CODE} '${BODYGOT}'"
    get "/s/c+d.txt"
    [ "${CODE}" = 200 ] && [ "${BODYGOT}" = "plus file" ] && pass "/s/c+d.txt: '+' is a plain character" \
        || fail "/s/c+d.txt: ${CODE} '${BODYGOT}'"
    for p in "/s/sub%2Fx.txt" "/s/sub%2fx.txt" "/s/a%00b.txt" "/s/bad%zz" "/s/trunc%2"; do
        get "${p}"
        if [ "${CODE}" = 400 ]; then pass "${p} is 400"
        else fail "${p}: ${CODE} '${BODYGOT}' (want 400)"; fi
    done
    for p in "/s/%2e%2e/secret.txt" "/s/%2E%2E/secret.txt" "/s/sub/../secret.txt"; do
        get "${p}"
        if [ "${CODE}" = 403 ] && [ "${BODYGOT}" != "SECRET" ]; then pass "${p} is 403"
        else fail "${p}: ${CODE} '${BODYGOT}' (want 403)"; fi
    done
    for p in /s/missing.txt /elsewhere; do
        get "${p}"
        case "${CT}" in *text/html*) cth=1 ;; *) cth=0 ;; esac
        if [ "${CODE}" = 404 ] && [ "${BODYGOT}" = "${BODY}" ] && [ "${cth}" = 1 ]; then
            pass "${p}: the setnotfound body, text/html"
        else fail "${p}: ${CODE} '${BODYGOT}' ${CT:-no content-type} (want 404 '${BODY}' text/html)"; fi
    done
    get /ok
    [ "${CODE}" = 200 ] && [ "${BODYGOT}" = ok ] && pass "/ok (getstatic) still 200" \
        || fail "/ok: ${CODE} '${BODYGOT}'"
fi
stop "${SRV}"; SRV=""

start unset
if [ -z "${PORT}" ]; then fail "server (no setnotfound) did not start"
else
    get /s/missing.txt
    if [ "${CODE}" = 404 ] && [ "${BODYGOT}" = "Not Found" ]; then pass "no setnotfound: a miss is still 404 'Not Found'"
    else fail "no setnotfound: ${CODE} '${BODYGOT}'"; fi
fi
stop "${SRV}"; SRV=""

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
