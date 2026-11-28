#!/usr/bin/env bash
# C343_router_serve_port.sh — router.serve binds the port it is given: 0 is a
# kernel-chosen port it reports, above 65535 is $err (story 135.26 item 2,
# aligned with http.serve's 135.24 item 2).
#
# THE DEFECT.  tk_router_serve_w (src/stdlib/router_glue.c) replaced a port
# of 0 or above 65535 with 8080, silently: router.serve(r;"";0) served on
# 8080, which the caller never asked for and could not learn, and
# router.serve(r;"";70000) did the same instead of failing.
#
# THE FIX.  The glue passes the port through.  router_serve (router.c) binds
# 0 as a kernel-chosen port and prints `DIAGNOSTIC: kind=server_listening
# port=N` on stderr once listening -- the line http.serve prints (135.24) --
# and its existing "port out of range" check makes 70000 the $err arm.
#
# WHAT IS ASSERTED:
#   1. port 0: the server reports a port, and a GET to that port is answered
#      by the registered handler
#   2. port 70000: router.serve takes $err at once (exit 7 from the $err arm)
#
# Negative control: C343_STDLIB=<dir> builds against another stdlib tree.
# Against main (18b9f73) both fail: port 0 reports nothing (it serves on
# 8080), and port 70000 does not return (it serves on 8080).
#
# Story: 135.26 (item 2), 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C343_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c343_XXXXXX")"
SRV=""
cleanup() { [ -n "${SRV}" ] && kill -9 "${SRV}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C343: router.serve binds the port it is given (135.26)"
echo "------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

cat > rs.tk <<'TOKE'
m=rs;
i=router:std.router;
i=args:std.args;
i=str:std.str;
f=h(req:i64):i64{ <router.ok("routed") };
f=main():i64{
  let r=router.new();
  router.get(r;"/";&h);
  let p=mt str.toint(mt args.get(1){$ok:v v;$err:e ""}) {$ok:v v; $err:e 0};
  <mt router.serve(r;"";p as u64) {$ok:v 0; $err:e 7}
};
TOKE
if ! "${TKC}" --allow-all --out rs_bin rs.tk > build.log 2>&1; then
    echo "  FAIL: the server does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

# ── 1. port 0 ───────────────────────────────────────────────────────────
./rs_bin 0 > s0.out 2> s0.err &
SRV=$!
tk_wait_until "${SRV}" tk_listening s0.err
PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9]*\)$/\1/p' s0.err | head -1)"
if [ -n "${PORT}" ] && [ "${PORT}" != "8080" ]; then
    BODY="$(python3 -c "
import urllib.request,sys
print(urllib.request.urlopen('http://127.0.0.1:${PORT}/', timeout=5).read().decode())" 2>&1)"
else
    BODY="(no port reported: $(head -c 200 s0.err))"
fi
expect "1  port 0: a kernel-chosen port is reported and serves the route" \
    "${BODY}" "routed"
kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; SRV=""

# ── 2. port 70000 ───────────────────────────────────────────────────────
./rs_bin 70000 > s1.out 2> s1.err &
SRV=$!
RC="running"
for _ in $(seq 1 30); do
    if ! kill -0 "${SRV}" 2>/dev/null; then wait "${SRV}"; RC=$?; break; fi
    sleep 0.1
done
if [ "${RC}" = "running" ]; then kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; fi
SRV=""
expect "2  port 70000: the \$err arm, at once" "exit ${RC}" "exit 7"

echo "------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
