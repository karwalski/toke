#!/usr/bin/env bash
# C1300_ws_client_peer_close_no_sigpipe.sh — a WebSocket server that closes
# before or while the client writes must give the client $err, never kill it
# with SIGPIPE (story 127.347).
#
# THE DEFECT.  src/stdlib/ws.c wrote every frame with plain write(2) on a
# socket that never had SO_NOSIGPIPE set, and nothing in the file mentioned
# SIGPIPE or MSG_NOSIGNAL.  ws.send / ws.sendbytes / ws.close to a peer that
# had already gone failed with EPIPE AND raised SIGPIPE, and the toke program
# died with exit status 141 instead of taking the $err arm — the client-side
# twin of 127.144 (server) and 127.341 (http client).  The fix sets the same
# per-socket guard on the ws client socket (SO_NOSIGPIPE where it exists) and
# writes every frame with send(..., MSG_NOSIGNAL) where that exists, through
# the helpers 127.341 uses for the http client (src/stdlib/tk_nosigpipe.h).
#
# WHY IT IS DETERMINISTIC.  Nothing here waits on a clock.
#   before — the server completes the handshake and closes.  The client
#            first calls ws.recv, which can only return once the server's FIN
#            has arrived (it takes $err on EOF), so the peer is known to be
#            gone before any write.  Then two 8 MiB ws.sendbytes and a ws.send.
#   while  — the server completes the handshake, reads the first two bytes of
#            the client's next frame and closes; the 8 MiB frame cannot fit in
#            the loopback buffers, so the rest of it always meets the closed
#            peer.
# Whether the kernel reports the dead peer as EPIPE (raises SIGPIPE) or
# ECONNRESET (does not) varies from run to run, so each case runs RUNS times,
# as C1260 does: the fixed client survives every run, and every send is $err.
# It can only pass by mistake, never fail by mistake.
#
# Negative control: C1300_STDLIB=<dir holding 75aa44a0's src/stdlib> — see the
# commit for the measured result.
#
# Story: 127.347

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C1300_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
RUNS="${C1300_RUNS:-12}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: compiler not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1300_XXXXXX")"; SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1

cat > srv.py <<'PY'
import socket, base64, hashlib
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(16)
print(s.getsockname()[1], flush=True)
while True:
    c, _ = s.accept()
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(4096)
            if not d: break
            buf += d
        head = buf.decode("latin1")
        key = ""
        for line in head.split("\r\n"):
            if line.lower().startswith("sec-websocket-key:"):
                key = line.split(":", 1)[1].strip()
        acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                   "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
        if " /while " in head.split("\r\n")[0] + " ":
            got = b""
            while len(got) < 2:               # the frame has started arriving
                d = c.recv(2 - len(got))
                if not d: break
                got += d
    except OSError:
        pass
    c.close()                                 # before: at once; while: mid-frame
PY
python3 srv.py > srv.port 2> srv.err & SRV=$!
for _ in $(seq 1 50); do [ -s srv.port ] && break; sleep 0.1; done
P="$(cat srv.port 2>/dev/null)"
if [ -z "${P}" ]; then echo "FAIL: the loopback server did not start"; cat srv.err; echo "C1300: 0 passed, 1 failed"; exit 1; fi

python3 -c "open('body.bin','wb').write(b'x' * (8 << 20))"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=ws:std.ws;
i=file:std.file;
i=args:std.args;
f=fail(msg:$str):i64{
  io.println(msg);
  <1
};
f=main():i64{
  let body=mt file.readbytes(args.get(3)) {$ok:b b; $err:e @()};
  let mode=args.get(2);
  let c=mt ws.connect("ws://127.0.0.1:\(args.get(1))/\(mode)") {$ok:k k; $err:e <fail("connect-err")};
  let pre=if(mode=="before"){mt ws.recv(c) {$ok:x "recv-ok "; $err:e "recv-err "}}el{""};
  let a=mt ws.sendbytes(c;body) {$ok:v "ok"; $err:e "err"};
  let b=mt ws.sendbytes(c;body) {$ok:v "ok"; $err:e "err"};
  let t=mt ws.send(c;"x") {$ok:v "ok"; $err:e "err"};
  ws.close(c);
  io.println("\(pre)\(a) \(b) \(t) closed");
  <0
};
TOKE
if ! "${TKC}" p.tk --out b > build.log 2>&1; then
    echo "FAIL: the program does not build"; head -6 build.log; echo "C1300: ${PASS} passed, 1 failed"; exit 1
fi

# Run with SIGPIPE at its default disposition whatever the caller's shell has.
for mode in before while; do
    want="err err err closed"; [ "${mode}" = "before" ] && want="recv-err ${want}"
    killed=0; good=0; other=""
    for _ in $(seq 1 "${RUNS}"); do
        out="$(trap - PIPE; ./b "${P}" "${mode}" body.bin --allow-net --allow-read 2>&1)"; rc=$?
        if [ "${rc}" -ge 128 ]; then killed=$((killed + 1))
        elif [ "${rc}" -eq 0 ] && [ "${out}" = "${want}" ]; then good=$((good + 1))
        else other="rc=${rc} out=${out}"; fi
    done
    expect "${mode}-peer-close-never-kills-the-client" "killed ${killed} of ${RUNS}" "killed 0 of ${RUNS}"
    expect "${mode}-every-write-to-the-closed-peer-is-err" "${good}${other:+ (${other})}" "$((RUNS - killed))"
done

echo "C1300: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
