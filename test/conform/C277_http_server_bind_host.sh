#!/usr/bin/env bash
# C277_http_server_bind_host.sh — a toke server can choose the address it
# binds (story 138.1, loke B1 / R1 / T-43).
#
# THE DEFECT.  http.serve / serveworkers / servetls / servetlsworkers took a
# port only and the glue passed a NULL host to bind_listen(), so every toke
# server listened on all interfaces.  http.listen("127.0.0.1:11442"; h)
# parsed past the host and dropped it: lsof showed *:11442.  loke's privacy
# proxy, which must be reachable from this machine only, could not say so.
#
# THE FIX.  The `...on` forms take the host first —
#   http.serveon(host; port)
#   http.serveworkerson(host; port; workers)
#   http.servetlson(host; port; cert; key)
#   http.servetlsworkerson(host; port; cert; key; workers)
# — and http.listen binds the host in its address.  A host is an IPv4
# literal, "localhost" (127.0.0.1), or ""/"*"/"0.0.0.0" for all interfaces;
# anything else is a start failure at stage "host" (exit 1), never a silent
# fall back to all interfaces.  DECIDED: the port-only forms keep binding all
# interfaces (tk_web_glue.c, server_host(), says why); case 6 pins that.
#
# WHAT IS ASSERTED (each server on port 0, the port read back from its
# `server_listening` line; the bound address from lsof, or ss on Linux):
#   1. serveworkerson("127.0.0.1";0;1) listens on 127.0.0.1 and answers
#   2. serveon("localhost";0) listens on 127.0.0.1 and answers
#   3. servetlson("127.0.0.1";0;cert;key) listens on 127.0.0.1, answers TLS
#   4. servetlsworkerson("127.0.0.1";0;cert;key;1) the same
#   5. http.listen("127.0.0.1:0"; h) listens on 127.0.0.1 and answers
#   6. serveworkers(0;1), the port-only form, listens on all interfaces
#   7. serveworkerson("0.0.0.0";0;1) listens on all interfaces
#   8. serveon("example.com";0) and serveon("::1";0): stage "host", exit 1
#
# Negative control: C277_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib the program does not link (no tk_http_serveon_w
# and friends), so every case fails; with server_host() made to return
# NULL for every host (measured), cases 1-5 read "*" and case 8 starts a
# server on all interfaces (killed after 3 s: 137) -- 2 passed, 7 failed.
#
# Story: 138.1
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C277_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if command -v lsof >/dev/null 2>&1; then LISTENERS=lsof
elif command -v ss >/dev/null 2>&1; then LISTENERS=ss
else echo "CONFORM-SKIP: neither lsof nor ss is installed to read a listener's address"; exit 0; fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "CONFORM-SKIP: no openssl(1) on this host to make a test certificate"; exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c277_XXXXXX")"
SRV=""
stop() { [ -n "$1" ] || return 0; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C277: a server binds the address it is given (138.1)"
echo "------------------------------------------------------"

openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 2 -nodes \
    -subj "/CN=localhost" > cert.log 2>&1

cat > srv.tk <<TOKE
m=srv;
i=http:std.http;
i=args:std.args;
i=io:std.io;
f=hi(req:i64):i64{ <http.resok("ok") };
f=main():i64{
  http.getstatic("/ok"; "ok");
  let mode=args.get(1);
  let host=args.get(2);
  let r=mut.0;
  if(mode=="on"){ r=http.serveon(host;0) }el{
  if(mode=="workerson"){ r=http.serveworkerson(host;0;1) }el{
  if(mode=="tlson"){ r=http.servetlson(host;0;"${WORK}/cert.pem";"${WORK}/key.pem") }el{
  if(mode=="tlsworkerson"){ r=http.servetlsworkerson(host;0;"${WORK}/cert.pem";"${WORK}/key.pem";1) }el{
  if(mode=="listen"){ r=http.listen("127.0.0.1:0";&hi) }el{
  r=http.serveworkers(0;1) }}}}};
  io.println("r=\(r)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out srv_bin srv.tk > build.log 2>&1; then
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

# serve NAME MODE HOST SCHEME WANT -- start, read the port, check the bound
# address is WANT:PORT and that / answers "ok" over SCHEME.
serve() {
    local name="$1" mode="$2" host="$3" scheme="$4" want="$5" port="" addr="" body=""
    ./srv_bin "${mode}" "${host}" > "${name}.out" 2> "${name}.err" &
    SRV=$!
    for _ in $(seq 1 80); do
        port="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "${name}.err" | head -1)"
        [ -n "${port}" ] && break
        kill -0 "${SRV}" 2>/dev/null || break
        sleep 0.1
    done
    if [ -n "${port}" ]; then
        for _ in $(seq 1 40); do
            body="$(curl -sk --max-time 2 "${scheme}://127.0.0.1:${port}/ok" 2>/dev/null)"
            [ "${body}" = ok ] && break
            sleep 0.1
        done
        addr="$(bound "${SRV}" "${port}")"
    fi
    stop "${SRV}"; SRV=""
    local got="${addr%:*} ${body:-no-answer}"
    if [ -n "${port}" ] && [ "${got}" = "${want} ok" ]; then
        echo "  PASS: ${name}: listens on ${addr}, answers over ${scheme}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"; echo "        expected: ${want} ok (port read back: '${port}')"; echo "        got:      ${got}"
        sed 's/^/        | /' "${name}.err" | head -4; FAIL=$((FAIL + 1))
    fi
}

serve "serveworkerson(\"127.0.0.1\";0;1)" workerson 127.0.0.1 http 127.0.0.1
serve "serveon(\"localhost\";0)" on localhost http 127.0.0.1
serve "servetlson(\"127.0.0.1\";0;cert;key)" tlson 127.0.0.1 https 127.0.0.1
serve "servetlsworkerson(\"127.0.0.1\";0;cert;key;1)" tlsworkerson 127.0.0.1 https 127.0.0.1
serve "listen(\"127.0.0.1:0\";h)" listen - http 127.0.0.1
serve "serveworkers(0;1), port only: all interfaces (the recorded default)" plain - http '*'
serve "serveworkerson(\"0.0.0.0\";0;1): all interfaces" workerson 0.0.0.0 http '*'

# 8. a host that is not an IPv4 literal or localhost is refused, not widened
for h in example.com ::1; do
    ./srv_bin on "${h}" > bad.out 2> bad.err &
    SRV=$!
    for _ in $(seq 1 30); do kill -0 "${SRV}" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "${SRV}" 2>/dev/null; then stop "${SRV}"; rc=137; else wait "${SRV}"; rc=$?; fi
    SRV=""
    got="${rc} $(grep -a '^r=' bad.out) $(grep -a -c '^DIAGNOSTIC: kind=server_start_failed fn="http.serveon" port=0 stage="host"$' bad.err) $(grep -a -c '^DIAGNOSTIC: kind=server_listening' bad.err)"
    if [ "${got}" = "1 r=-1 1 0" ]; then
        echo "  PASS: serveon(\"${h}\";0): stage host, exit 1, nothing bound"; PASS=$((PASS + 1))
    else
        echo "  FAIL: serveon(\"${h}\";0)"; echo "        expected: 1 r=-1 1 0"; echo "        got:      ${got}"
        sed 's/^/        | /' bad.err | head -4; FAIL=$((FAIL + 1))
    fi
done

echo "------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
