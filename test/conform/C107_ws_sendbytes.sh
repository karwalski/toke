#!/usr/bin/env bash
# C107_ws_sendbytes.sh — ws.sendbytes sends a binary frame (story 127.150).
#
# THE DEFECT.  stdlib/ws.tki exports ws.sendbytes and docs/stdlib/ws.md lists
# it in the function table with no caveat, but no tk_ws_sendbytes_w existed,
# so any program naming it died at ld on a bare _tk_ws_sendbytes_w.  The C
# core has had ws_sendbytes() — a WS_BINARY frame through the same encoder
# ws.send uses — all along; the wrapper was the missing door.  It was on
# scripts/check_tki_skiplist.txt and docs/reference/tki-quarantine.md, which
# the reader-facing page did not mention.
#
# WHAT THIS DRIVES.  A minimal RFC 6455 echo server on loopback (no external
# network).  It unmasks each client frame and sends it back with the same
# opcode.  A real toke client sends @(0;1;2;255) with ws.sendbytes and reads
# the echo with ws.recv, so the bytes are checked after crossing the wire
# twice: opcode 2 (binary, not text), length 4, a leading NUL kept, 255 kept.
# Then sendbytes on a closed connection must take the $err arm.
#
# Negative control: delete tk_ws_sendbytes_w from src/stdlib/ws_glue.c — the
# client no longer links (undefined tk_ws_sendbytes_w).
#
# Story: 127.150, 127.565

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c107_XXXXXX)"
WSS=""
cleanup() {
    [ -n "${WSS}" ] && { kill "${WSS}" 2>/dev/null; wait "${WSS}" 2>/dev/null; }
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "C107: ws.sendbytes sends a binary frame"
echo "---------------------------------------"

cat > "${WORK}/echo.py" <<'PY'
import socket, base64, hashlib, struct, sys, threading
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[1])))
srv.listen(5)

def recv_exact(c, n):
    b = b""
    while len(b) < n:
        chunk = c.recv(n - len(b))
        if not chunk:
            raise EOFError
        b += chunk
    return b

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
    try:
        while True:
            h = recv_exact(c, 2)
            opcode, masked, n = h[0] & 0x0F, h[1] & 0x80, h[1] & 0x7F
            if n == 126:
                n = struct.unpack(">H", recv_exact(c, 2))[0]
            elif n == 127:
                n = struct.unpack(">Q", recv_exact(c, 8))[0]
            mask = recv_exact(c, 4) if masked else b"\0\0\0\0"
            p = bytes(x ^ mask[i % 4] for i, x in enumerate(recv_exact(c, n)))
            if opcode == 8:
                break
            sys.stderr.write("frame opcode=%d len=%d\n" % (opcode, len(p)))
            sys.stderr.flush()
            c.sendall(bytes([0x80 | opcode, len(p)]) + p)   # payloads here are < 126
    except Exception:
        pass
    c.close()

while True:
    conn, _ = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
PY

PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
python3 "${WORK}/echo.py" "${PORT}" > "${WORK}/echo.log" 2>&1 &
WSS=$!
up=0
tk_wait_until "${WSS}" tk_port_open "${PORT}" && up=1
if [ "${up}" -ne 1 ]; then
    echo "  FAIL: the loopback WebSocket server never came up"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

cat > "${WORK}/cli.tk" <<EOF
m=app;
i=ws:std.ws;
i=io:std.io;
f=fail(msg:\$str):i64{
  io.println(msg);
  <1
};
f=main():i64{
  let conn=mt ws.connect("ws://127.0.0.1:${PORT}/echo"){\$ok:c c;\$err:e <fail("connect failed")};
  mt ws.sendbytes(conn;@(0;1;2;255)){\$ok:v v;\$err:e <fail("sendbytes took the err arm")};
  let m=mt ws.recv(conn){\$ok:m m;\$err:e <fail("recv took the err arm")};
  io.println(m.opcode as \$str);
  io.println(m.payload.len as \$str);
  io.println(m.payload.get(0) as \$str);
  io.println(m.payload.get(3) as \$str);
  ws.close(conn);
  let after=mt ws.sendbytes(conn;@(7)){\$ok:v "sent";\$err:e "err"};
  io.println(after);
  <0
};
EOF

if ! (cd "${WORK}" && "${TKC}" --allow-all -o cli.bin cli.tk > build.log 2>&1) \
   || [ ! -x "${WORK}/cli.bin" ]; then
    echo "  FAIL: a client calling ws.sendbytes did not build"
    grep -a -o 'undefined symbol[^"]*' "${WORK}/build.log" | head -3 | sed 's/^/        /'
    echo "Results: 0 passed, 1 failed"
    exit 1
fi
echo "  PASS: a client calling ws.sendbytes builds and links"
PASS=$((PASS + 1))

"${WORK}/cli.bin" > "${WORK}/out.txt" 2>&1
rc=$?
got="$(tr '\n' ' ' < "${WORK}/out.txt" | sed 's/ *$//')"
expect_() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
expect_ "the server received one BINARY frame of 4 bytes" \
    "$(grep -a -c 'frame opcode=2 len=4' "${WORK}/echo.log")" "1"
expect_ "the echo reads back as opcode 2, len 4, bytes[0]=0, bytes[3]=255; closed conn is \$err" \
    "${rc} ${got}" "0 2 4 0 255 err"

echo "---------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
