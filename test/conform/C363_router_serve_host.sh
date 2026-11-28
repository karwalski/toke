#!/usr/bin/env bash
# C363_router_serve_host.sh — router.serve binds the host it is given (story
# 135.28 item 2, the router side of 138.1).
#
# THE DEFECT.  router.serve(r; host; port) took a host and router_serve
# (src/stdlib/router.c) discarded it -- `(void)host` -- and bound INADDR_ANY,
# so router.serve(r;"127.0.0.1";p) was reachable from every interface, and a
# host that was not an address at all was accepted silently.
#
# THE FIX.  router_serve applies http.serveon's rules (138.1): an IPv4
# literal, "localhost" (127.0.0.1), or ""/"*"/"0.0.0.0" for all interfaces;
# anything else is router.serve's $err before anything is bound.
#
# WHAT IS ASSERTED (the bound address read back with lsof, or ss on Linux):
#   1. "127.0.0.1" listens on 127.0.0.1 and answers
#   2. "localhost" listens on 127.0.0.1 and answers
#   3. NEGATIVE CONTROL -- "0.0.0.0" and "" still listen on all interfaces,
#      so "always loopback" cannot pass
#   4. "not-a-host" takes $err at once (exit 7), and nothing listens
#
# Negative control of the suite: C363_STDLIB=<dir> builds against another
# stdlib tree.  Against main 73f7466 cases 1, 2 and 4 fail (every host
# binds *, and "not-a-host" serves).
#
# Story: 135.28 (item 2), 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C363_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if command -v lsof >/dev/null 2>&1; then LISTENERS=lsof
elif command -v ss >/dev/null 2>&1; then LISTENERS=ss
else echo "SKIP: neither lsof nor ss is available to read the bound address"; echo "Results: 0 passed, 0 failed"; exit 0; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c363_XXXXXX")"
cleanup() { [ -n "${SRV}" ] && kill -9 "${SRV}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C363: router.serve binds the host it is given (135.28)"
echo "------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

cat > rs.tk <<'TOKE'
m=rs;
i=router:std.router;
i=args:std.args;
f=h(req:i64):i64{ <router.ok("routed") };
f=main():i64{
  let r=router.new();
  router.get(r;"/";&h);
  <mt router.serve(r;mt args.get(1){$ok:v v;$err:e ""};0) {$ok:v 0; $err:e 7}
};
TOKE
if ! "${TKC}" --allow-all --out rs_bin rs.tk > build.log 2>&1; then
    echo "  FAIL: the server does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

bound() {   # bound PID PORT -> the local address the process listens on
    if [ "${LISTENERS}" = lsof ]; then
        lsof -nP -a -p "$1" -iTCP:"$2" -sTCP:LISTEN 2>/dev/null | awk 'NR>1{print $9; exit}'
    else
        ss -ltnH "sport = :$2" 2>/dev/null | awk '{print $4; exit}' | sed 's/^0\.0\.0\.0:/*:/'
    fi
}

# serve HOST -> "<bound address without port> <body>"
serve() {
    local port="" addr="" body=""
    ./rs_bin "$1" > srv.out 2> srv.err &
    SRV=$!
    tk_wait_until "${SRV}" tk_listening srv.err
    port="$(tk_listening_port srv.err)"
    if [ -n "${port}" ]; then
        body="$(curl -s --max-time 3 "http://127.0.0.1:${port}/" 2>/dev/null)"
        addr="$(bound "${SRV}" "${port}")"
    fi
    kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""
    echo "${addr%:*} ${body:-no-answer}"
}

expect "1  \"127.0.0.1\": listens on 127.0.0.1" "$(serve 127.0.0.1)" "127.0.0.1 routed"
expect "2  \"localhost\": listens on 127.0.0.1" "$(serve localhost)" "127.0.0.1 routed"
expect "3a NEGATIVE CONTROL: \"0.0.0.0\" still binds all interfaces" "$(serve 0.0.0.0)" "* routed"
expect "3b NEGATIVE CONTROL: \"\" still binds all interfaces" "$(serve '')" "* routed"

./rs_bin not-a-host > bad.out 2> bad.err & SRV=$!
rc=""
for _ in $(seq 1 30); do
    if ! kill -0 "${SRV}" 2>/dev/null; then wait "${SRV}"; rc=$?; break; fi
    sleep 0.1
done
[ -z "${rc}" ] && { kill -9 "${SRV}" 2>/dev/null; rc="still running"; }
SRV=""
expect "4  \"not-a-host\": \$err at once, nothing listening" \
    "${rc} $(grep -c 'server_listening' bad.err)" "7 0"

echo "------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
