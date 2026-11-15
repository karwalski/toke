#!/usr/bin/env bash
# C1881_cap_scope_net.sh -- the net class scoped by direction, host and port
# (story 138.19, D1; design note §1, §3.2).
#
# THE DEFECT (loke T-59 points 1 and 2).  `net` was one grant covering
# listening on any address and port AND connecting to any host: a binary
# granted `net = "10.255.255.1:9"` bound a loopback port, and nothing could
# say "listen on loopback only" or "connect to this host only".
#
# THE CHANGE.  A net grant is the whole class or a list of direction-prefixed
# scopes: connect:HOST[:PORT] (HOST a DNS name, *.domain, an IP literal or a
# CIDR block; matched against the name the program passed, before DNS) and
# listen:ADDR:PORT (the exact bind address).  net.portavailable binds
# 127.0.0.1, so it is a listen check; net.accept is checked on the address
# its listener is bound to.
#
# WHAT IS ASSERTED (servers bind 127.0.0.1 ports 48110-48119 only; the
# connects go to closed 127.0.0.1 ports, so an admitted connect ends in a
# refused connection, never in traffic; nothing leaves the machine)
#   1. listen:127.0.0.1:P admits net.portavailable(P), denies P+1 and a bind
#      of another address on P (0.0.0.0 vs 127.0.0.1 is in the unit test)
#   2. connect:127.0.0.1:P admits ws.connect to that host and port, denies
#      another port, and denies the name `localhost` (no DNS)
#   3. connect:127.0.0.0/8 admits an IP literal inside it
#   4. a connect-only grant denies a listen; a listen-only grant denies a
#      connect
#   5. listen:127.0.0.1:P admits net.listen and then net.accept of a client
#      connection on it (the accept is checked on the listener's address)
#   6. the whole class (--allow-net) still admits all of it
# (Wildcard and IPv6 matching, which would need DNS or v6 here, are in the
# broker's unit test, test/stdlib/test_capabilities.c.)
#
# NEGATIVE CONTROL: TKC=<a v3.2.0 toke> -- every denial FAILS (the scope is
# ignored, or, for the connect: form, the flag grants the whole class).
#
# Story: 138.19
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1881_XXXXXX")"
SRV_PID=""
cleanup() { [ -n "${SRV_PID}" ] && kill "${SRV_PID}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
echo "C1881: net scoped by direction, host and port (138.19 D1)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/netprobe.tk" <<'EOF'
m=netprobe;
i=io:std.io;
i=args:std.args;
i=net:std.net;
i=ws:std.ws;
i=s:std.str;
f=main():i64{
  let op=mt args.get(1) {$ok:v v;$err:e ""};
  let a=mt args.get(2) {$ok:v v;$err:e ""};
  if(op=="avail"){
    let n=mt s.toint(a) {$ok:v v;$err:e 0};
    io.println("avail-done \(net.portavailable(n))");
  }el{};
  if(op=="listen"){
    let l=net.listen(a);
    io.println("listen-done \(l>0)");
  }el{};
  if(op=="connect"){
    let r=mt ws.connect(a) {$ok:c 1;$err:e 2};
    io.println("connect-done \(r)");
  }el{};
  if(op=="accept"){
    let l=net.listen(a);
    io.println("listening \(l>0)");
    let c=net.accept(l);
    io.println("accept-done \(c>0)");
  }el{};
  <0
};
EOF
if ! "${TKC}" --out "${WORK}/netprobe" "${WORK}/netprobe.tk" >"${WORK}/build.log" 2>&1; then
    echo "  FAIL: probe did not build"; sed 's/^/      | /' "${WORK}/build.log" | head -5; exit 1
fi
P="${WORK}/netprobe"

run() {   # LABEL admit|deny ARGS...
    local label="$1" want="$2"; shift 2
    local rc=0
    perl -e 'alarm 20; exec @ARGV or exit 127' "$@" >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    if [ "${want}" = admit ] && [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: admitted ($(head -1 "${WORK}/o"))"
    elif [ "${want}" = deny ] && [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002: capability 'net'" "${WORK}/e" \
         && ! /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: denied ($(head -1 "${WORK}/e" | cut -c1-100))"
    else
        fail "${label}: wanted ${want}, got rc=${rc}"; sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -4
    fi
}

# 1 listen scopes
run "1a portavailable(48110) under listen:127.0.0.1:48110" admit "${P}" avail 48110 --allow-net=listen:127.0.0.1:48110
run "1b portavailable(48111) under listen:127.0.0.1:48110" deny "${P}" avail 48111 --allow-net=listen:127.0.0.1:48110
# (another loopback address, not 0.0.0.0: on a build that ignores the scope
# the negative control would otherwise bind every interface)
run "1c net.listen 127.0.0.2:48110 under listen:127.0.0.1:48110" deny "${P}" listen 127.0.0.2:48110 --allow-net=listen:127.0.0.1:48110
run "1d net.listen 127.0.0.1:48112 under listen:127.0.0.1:*" admit "${P}" listen 127.0.0.1:48112 '--allow-net=listen:127.0.0.1:*'
# 2 connect scopes (closed ports: an admitted connect is refused, exit 0)
run "2a ws.connect 127.0.0.1:48113 under connect:127.0.0.1:48113" admit "${P}" connect ws://127.0.0.1:48113/ --allow-net=connect:127.0.0.1:48113
run "2b ws.connect 127.0.0.1:48114 (another port)" deny "${P}" connect ws://127.0.0.1:48114/ --allow-net=connect:127.0.0.1:48113
run "2c ws.connect localhost:48113 (a name; an IP scope never matches one)" deny "${P}" connect ws://localhost:48113/ --allow-net=connect:127.0.0.1:48113
run "2d ws.connect localhost:48113 under connect:localhost:48113" admit "${P}" connect ws://localhost:48113/ --allow-net=connect:localhost:48113
# 3 CIDR
run "3 ws.connect 127.0.0.1:48113 under connect:127.0.0.0/8" admit "${P}" connect ws://127.0.0.1:48113/ --allow-net=connect:127.0.0.0/8
# 4 direction
run "4a listen under a connect-only grant" deny "${P}" avail 48115 --allow-net=connect:127.0.0.1:48115
run "4b connect under a listen-only grant" deny "${P}" connect ws://127.0.0.1:48115/ --allow-net=listen:127.0.0.1:48115
# 5 listen + accept
rm -f "${WORK}/acc.out"
( perl -e 'alarm 20; exec @ARGV or exit 127' "${P}" accept 127.0.0.1:48116 --allow-net=listen:127.0.0.1:48116 \
    >"${WORK}/acc.out" 2>"${WORK}/acc.err" ) &
SRV_PID=$!
for _ in $(seq 50); do /usr/bin/grep -a -q "^listening" "${WORK}/acc.out" 2>/dev/null && break; sleep 0.1; done
python3 -I -c 'import socket,sys; s=socket.create_connection(("127.0.0.1",48116),3); s.close()' 2>/dev/null
wait "${SRV_PID}" 2>/dev/null; arc=$?; SRV_PID=""
if [ "${arc}" -eq 0 ] && /usr/bin/grep -a -q "accept-done true" "${WORK}/acc.out"; then
    pass "5 net.listen + net.accept under listen:127.0.0.1:48116: $(tr '\n' ' ' <"${WORK}/acc.out")"
else
    fail "5 net.listen + net.accept: rc=${arc}"; sed 's/^/      | /' "${WORK}/acc.out" "${WORK}/acc.err" | head -4
fi
# 6 whole class
run "6a --allow-net (whole class) portavailable" admit "${P}" avail 48117 --allow-net
run "6b --allow-net (whole class) connect" admit "${P}" connect ws://127.0.0.1:48118/ --allow-net

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
