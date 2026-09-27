#!/usr/bin/env bash
# S010_ws_recv_returns_wsmsg.sh — ws.recv returns the record it is declared to
# return (story 127.145).
#
# THE DEFECT.  `stdlib/ws.tki` declares
#
#     ws.recv : $wsconn -> $wsmsg!$wserr
#     $wsmsg  { payload: @($byte); fin: bool; opcode: u64 }
#
# and `tk_ws_recv_w` returned a bare `char *` — the text payload copied into a
# NUL-terminated string.  A toke record is a block of i64 slots, so
# `msg.payload` GEP'd slot 0 of a char buffer and loaded it as a pointer.
# Measured against a live WebSocket server with a clean RFC 6455 handshake and
# `"hello"` delivered: SIGBUS, **exit 138**.  The Combined Example in
# docs/stdlib/ws.md did exactly this, so the documented way to use the module
# crashed.
#
# It was the same family as 127.112 pointing the other way: there a fabricated
# toke struct was read as a C handle, here a C string was handed out as a toke
# struct.
#
# AND IT LOST MORE THAN THE FIELDS.  The old wrapper returned 0 — which the
# caller sees as the `$err` arm — for every frame that was not a non-empty
# TEXT frame.  So a binary frame was indistinguishable from a failure, leaving
# `ws.sendbytes` with no way to receive what it sends, and an empty text frame
# was reported as an error.  Case 2 below is the regression test for that, and
# it is why the fix builds a real `$wsmsg` rather than narrowing the declared
# type to `str!$wserr`: narrowing would have matched the defect and made the
# losses permanent.
#
# WHAT THIS DRIVES.  A real WebSocket server on loopback — no external network
# — performing a real handshake and sending one text frame and one binary
# frame with an embedded NUL byte, and a real toke client reading every field
# of both.  Exact expected values, no counting and no thresholds, so machine
# load cannot change the result.
#
# Story: 127.145

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_s010_XXXXXX)"
WSS=""
cleanup() {
    [ -n "${WSS}" ] && { kill "${WSS}" 2>/dev/null; wait "${WSS}" 2>/dev/null; }
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "S010: ws.recv returns a \$wsmsg, not a string pointer"
echo "--------------------------------------"

# ── a minimal RFC 6455 server, loopback only ────────────────────────────
cat > "${WORK}/wsd.py" <<'PY'
import socket, base64, hashlib, struct, sys, threading
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
port = int(sys.argv[1])
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(5)

def frame(payload, opcode=1):
    out = bytearray([0x80 | opcode])
    n = len(payload)
    if n < 126:
        out.append(n)
    elif n < 65536:
        out.append(126); out += struct.pack(">H", n)
    else:
        out.append(127); out += struct.pack(">Q", n)
    return bytes(out) + payload

def handle(c):
    data = b""
    while b"\r\n\r\n" not in data:
        b = c.recv(4096)
        if not b:
            c.close(); return
        data += b
    key = ""
    for line in data.decode("latin1").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
    c.sendall(frame(b"hello"))                              # text, 5 bytes
    c.sendall(frame(b"\x00\x01\x02\xff", opcode=2))         # binary, NUL first
    try:
        while True:
            if not c.recv(4096):
                break
    except Exception:
        pass
    c.close()

while True:
    conn, _ = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
PY

PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
python3 "${WORK}/wsd.py" "${PORT}" >"${WORK}/wsd.log" 2>&1 &
WSS=$!
up=0
for _ in $(seq 1 80); do
    python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.3)
sys.exit(0 if s.connect_ex(('127.0.0.1',${PORT}))==0 else 1)" && { up=1; break; }
    sleep 0.25
done
if [ "${up}" -ne 1 ]; then
    echo "  FAIL: the loopback WebSocket server never came up"
    sed 's/^/      /' "${WORK}/wsd.log" | head -6
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

# ── the client: reads every field of both frames ────────────────────────
cat > "${WORK}/cli.tk" <<EOF
m=app;
i=ws:std.ws;
i=io:std.io;
f=fail(msg:\$str):i64{
  io.println(msg);
  <1
};
f=main():i64{
  let conn=mt ws.connect("ws://127.0.0.1:${PORT}/chat"){\$ok:c c;\$err:e <fail("connect failed")};
  let a=mt ws.recv(conn){\$ok:m m;\$err:e <fail("recv 1 took the err arm")};
  io.println(a.opcode as \$str);
  io.println(a.fin as \$str);
  io.println(a.payload.len as \$str);
  let b=mt ws.recv(conn){\$ok:m m;\$err:e <fail("recv 2 took the err arm — a binary frame is not an error")};
  io.println(b.opcode as \$str);
  io.println(b.payload.len as \$str);
  io.println(b.payload.get(0) as \$str);
  io.println(b.payload.get(3) as \$str);
  ws.close(conn);
  <0
};
EOF

if ! (cd "${WORK}" && "${TKC}" --allow-all -o cli.bin cli.tk >build.log 2>&1) \
   || [ ! -x "${WORK}/cli.bin" ]; then
    echo "  FAIL: the ws.recv client did not build"
    sed 's/^/      /' "${WORK}/build.log" | head -8
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

"${WORK}/cli.bin" >"${WORK}/out.txt" 2>&1
rc=$?
got="$(tr '\n' ' ' < "${WORK}/out.txt" | sed 's/  *$//')"

# opcode fin len  |  opcode len byte0 byte3
WANT="1 true 5 2 4 0 255"  # fin is a bool field: true since 127.94 (127.15 decision)

if [ "${rc}" -ne 0 ]; then
    echo "  FAIL: the client exited ${rc} (138 = SIGBUS, the defect itself)"
    echo "        output: [${got}]"
    FAIL=$((FAIL + 1))
else
    if [ "${got}" = "${WANT}" ]; then
        echo "  PASS: text frame read as opcode=1 fin=true payload.len=5"
        PASS=$((PASS + 1))
        echo "  PASS: binary frame delivered as \$ok with opcode=2, len=4, an embedded NUL preserved"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: fields read back as [${got}], expected [${WANT}]"
        echo "        (opcode fin payload.len | opcode payload.len payload[0] payload[3])"
        FAIL=$((FAIL + 1))
    fi
fi

# ── the interface must not be quietly narrowed to match the runtime ─────
if awk '/"name": "ws.recv"/ && /wsmsg!wserr/ { found = 1 } END { exit !found }' \
       "${REPO_ROOT}/stdlib/ws.tki"; then
    echo "  PASS: stdlib/ws.tki still declares ws.recv as wsmsg!wserr"
    PASS=$((PASS + 1))
else
    echo "  FAIL: stdlib/ws.tki no longer declares ws.recv as wsmsg!wserr"
    FAIL=$((FAIL + 1))
fi

kill "${WSS}" 2>/dev/null; wait "${WSS}" 2>/dev/null; WSS=""

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
