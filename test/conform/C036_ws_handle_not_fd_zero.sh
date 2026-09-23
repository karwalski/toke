#!/usr/bin/env bash
# C036_ws_handle_not_fd_zero.sh — a fabricated $wsconn must never become a
# live connection on file descriptor 0 (story 127.112).
#
# THE DEFECT.  `$wsconn` is an opaque handle: the i64 that crosses the ABI is
# the address of a C `WsConn`.  Nothing checked that the address was one the
# runtime had issued, and `docs/stdlib/ws.md` line 54 *documented* fabricating
# one as the `$err` arm of ws.connect:
#
#     let conn=mt ws.connect(...) {$ok:c c;$err:e $wsconn{id:0;ready:false}};
#
# That literal is a zeroed block.  The old WsConn layout was fd@0 / mask@4 /
# closed@8, so the cast landed INSIDE the block, never faulted, and read
# fd == 0, mask == 0, closed == 0.  `closed == 0` means OPEN.  So ws.send
# encoded a real RFC 6455 text frame and wrote it to file descriptor 0 — and
# returned the `$ok` arm.  Measured on this tree before the fix, running the
# documented example with stdin opened read-write on a file:
#
#     $ ./ws_doc.bin 0<> sink.bin
#     $ xxd sink.bin
#     00000000: 8105 6865 6c6c 6f88 00        ..hello..
#
# `8105 68656c6c6f` is FIN|text, length 5, "hello".  The trailing `8800` is
# the close frame from the ws.close(conn) two lines later — which also
# close()d the caller's standard input.
#
# WHY IT LOOKED INCONSISTENT, AND WHY THAT IS PINNED HERE.  The arm taken
# depends on what fd 0 happens to be.  A read-write file or a tty accepts the
# write, so ws.send reports SUCCESS and, interactively, the frame lands on the
# user's screen.  A pipe or a read-only /dev/null rejects it, so ws.send
# reports `$err` — which is why two earlier observers disagreed about the
# symptom and the defect survived as "suspected".  Cases 1 and 2 therefore
# assert BOTH halves on a read-write file: the honest arm *and* an empty file.
# Asserting the arm alone would have passed before the fix on a pipe.
#
# WHAT THE FIX IS.  WsConn now carries WS_CONN_MAGIC as its first field and
# every entry point runs ws_conn_valid(), which checks the magic and, for an
# open handle, that the descriptor really is a socket.  A foreign struct is
# rejected and the call fails honestly.
#
# CASE 6 IS WHAT STOPS THE FIX BEING A CONSTANT.  A "fix" that made ws.send
# always return `$err` would pass cases 1-5 and be far worse than the defect,
# because it would silently break every real client.  Case 6 stands up a
# loopback WebSocket server, completes a real handshake, and requires that the
# payload arrive — the frame must still be sent when the handle is genuine.
#
# Story: 127.112

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c036_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C036: a fabricated \$wsconn never becomes a live connection on fd 0"
echo "--------------------------------------"

# build SRC -> run.bin.  Returns non-zero and reports if it does not build.
build() {
    local src="$1"
    printf '%s\n' "${src}" > run.tk
    rm -f run.bin
    local out rc=0
    out="$("${TKC}" --allow-all --diag-json -o run.bin run.tk 2>&1)" || rc=$?
    if [ ! -x run.bin ]; then
        echo "      did not compile (exit ${rc})"
        printf '%s\n' "${out}" | grep -v '"severity":"warning"' | sed 's/^/      /' | head -4
        return 1
    fi
    return 0
}

# fd0_case NAME EXPECTED_STDERR SOURCE
#
# Runs the program with stdin opened READ-WRITE on a fresh empty file — the
# configuration that made the defect visible — and asserts two independent
# things: the reported arm, and that not one byte reached fd 0.
fd0_case() {
    local name="$1" expected="$2" src="$3"
    if ! build "${src}"; then
        echo "  FAIL: ${name}"
        FAIL=$((FAIL + 1)); return
    fi
    : > fd0.sink
    local got
    got="$(./run.bin 0<> fd0.sink 2>&1)"
    local n
    n="$(wc -c < fd0.sink | tr -d ' ')"
    rm -f run.bin
    if [ "${n}" != "0" ]; then
        echo "  FAIL: ${name}: ${n} bytes written to file descriptor 0:"
        xxd fd0.sink | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1)); return
    fi
    if [ "${got}" != "${expected}" ]; then
        echo "  FAIL: ${name}: expected [${expected}], got [${got}]"
        FAIL=$((FAIL + 1)); return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ════════════════════════════════════════════════════════════════════════
# 1. THE DOCUMENTED EXAMPLE.  ws.md:54 as it shipped.  Nothing listens on
#    port 8080, so ws.connect fails and the fabricated handle is what ws.send
#    receives.  Before the fix: "$ok" and 7 bytes in the file.
# ════════════════════════════════════════════════════════════════════════
fd0_case "documented \$wsconn{id:0;ready:false}: ws.send reports \$err, writes nothing to fd 0" \
    'ws.send -> $err' '
m=t;
i=ws:std.ws;
i=io:std.io;
f=main():i64{
  let conn=mt ws.connect("ws://127.0.0.1:1/chat"){$ok:c c;$err:e $wsconn{id:0;ready:false}};
  mt ws.send(conn;"hello"){$ok:v io.eprintln("ws.send -> $ok");$err:e io.eprintln("ws.send -> $err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 2. ws.close ON THE FABRICATION.  The same documented example closes the
#    handle, which was close(0) — it took away the caller's standard input,
#    after writing an `8800` close frame to it.  Reading a line back afterwards
#    is the liveness probe: with fd 0 closed io.readln() yields the empty
#    string, so "read:PROBE" is reachable only if fd 0 survived.  (That is
#    also why this case seeds the file instead of leaving it empty, and why it
#    checks the byte count separately — a close frame in the file would show
#    up as bytes even though the probe still reads PROBE back from offset 0.)
# ════════════════════════════════════════════════════════════════════════
if build '
m=t;
i=ws:std.ws;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let conn=mt ws.connect("ws://127.0.0.1:1/chat"){$ok:c c;$err:e $wsconn{id:0;ready:false}};
  ws.close(conn);
  let line=io.readln();
  io.eprintln(s.concat("read:";line));
  <0
};'; then
    printf 'PROBE\n' > fd0.sink
    got="$(./run.bin 0<> fd0.sink 2>&1)"
    n="$(wc -c < fd0.sink | tr -d ' ')"
    rm -f run.bin
    if [ "${got}" = "read:PROBE" ] && [ "${n}" = "6" ]; then
        echo "  PASS: ws.close on a fabricated handle neither writes to nor closes fd 0"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ws.close on a fabricated handle: probe said [${got}], fd 0 file is ${n} bytes (want read:PROBE / 6)"
        xxd fd0.sink | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: ws.close on a fabricated handle"
    FAIL=$((FAIL + 1))
fi

# ════════════════════════════════════════════════════════════════════════
# 3. NOT JUST THE ZEROED ONE.  A guard that only rejected an all-zero block
#    would pass case 1 and still hand any other struct to ws_send_all.  The
#    magic is checked, not the zeroness.
# ════════════════════════════════════════════════════════════════════════
fd0_case "a non-zero fabricated \$wsconn is rejected too" \
    'ws.send -> $err' '
m=t;
i=ws:std.ws;
i=io:std.io;
f=main():i64{
  let conn=mt ws.connect("ws://127.0.0.1:1/chat"){$ok:c c;$err:e $wsconn{id:7;ready:true}};
  mt ws.send(conn;"hello"){$ok:v io.eprintln("ws.send -> $ok");$err:e io.eprintln("ws.send -> $err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 4. THE READ SIDE.  ws.recv on the fabrication called read(0, ...) — it ate
#    the caller's standard input and then parsed it as a WebSocket frame.
# ════════════════════════════════════════════════════════════════════════
if build '
m=t;
i=ws:std.ws;
i=io:std.io;
f=main():i64{
  let conn=mt ws.connect("ws://127.0.0.1:1/chat"){$ok:c c;$err:e $wsconn{id:0;ready:false}};
  mt ws.recv(conn){$ok:m io.eprintln("ws.recv -> $ok");$err:e io.eprintln("ws.recv -> $err")};
  <0
};'; then
    printf 'DO NOT EAT ME' > fd0.sink
    got="$(./run.bin 0<> fd0.sink 2>&1)"
    left="$(cat fd0.sink)"
    rm -f run.bin
    if [ "${got}" = 'ws.recv -> $err' ] && [ "${left}" = "DO NOT EAT ME" ]; then
        echo "  PASS: ws.recv on a fabricated handle reports \$err and does not read fd 0"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ws.recv on a fabricated handle: arm [${got}], fd 0 holds [${left}]"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: ws.recv on a fabricated handle"
    FAIL=$((FAIL + 1))
fi

# ════════════════════════════════════════════════════════════════════════
# 5. BROADCAST.  It takes an array of handles and skipped only the NULL
#    slots, so a fabricated one in the array reached ws_send the same way.
# ════════════════════════════════════════════════════════════════════════
fd0_case "ws.broadcast over fabricated handles writes nothing to fd 0" \
    'done' '
m=t;
i=ws:std.ws;
i=io:std.io;
f=main():i64{
  let a=mt ws.connect("ws://127.0.0.1:1/chat"){$ok:c c;$err:e $wsconn{id:0;ready:false}};
  let b=mt ws.connect("ws://127.0.0.1:1/chat"){$ok:c c;$err:e $wsconn{id:0;ready:false}};
  ws.broadcast(@(a;b);"bye");
  io.eprintln("done");
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 6. THE NEGATIVE CONTROL.  A real handle must still send.  Without this,
#    "ws.send always fails" passes every case above.
# ════════════════════════════════════════════════════════════════════════
if ! command -v python3 >/dev/null 2>&1; then
    echo "  FAIL: python3 not available — cannot run the negative control"
    FAIL=$((FAIL + 1))
else
    cat > wsserver.py <<'PYEOF'
import socket, hashlib, base64, struct, sys
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0))
srv.listen(1)
sys.stdout.write("%d\n" % srv.getsockname()[1]); sys.stdout.flush()
srv.settimeout(20)
c, _ = srv.accept()
c.settimeout(20)
req = b""
while b"\r\n\r\n" not in req:
    d = c.recv(4096)
    if not d: sys.exit(1)
    req += d
key = b""
for line in req.split(b"\r\n"):
    if line.lower().startswith(b"sec-websocket-key:"):
        key = line.split(b":", 1)[1].strip()
acc = base64.b64encode(hashlib.sha1(key + GUID).digest())
c.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
          b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + acc + b"\r\n\r\n")
h = c.recv(2)
if len(h) < 2: sys.exit(1)
ln = h[1] & 0x7F
masked = h[1] & 0x80
if ln == 126: ln = struct.unpack(">H", c.recv(2))[0]
mk = c.recv(4) if masked else b""
pl = b""
while len(pl) < ln:
    d = c.recv(ln - len(pl))
    if not d: break
    pl += d
if masked: pl = bytes(b ^ mk[i % 4] for i, b in enumerate(pl))
sys.stdout.write("%s\n" % pl.decode("utf-8", "replace")); sys.stdout.flush()
PYEOF
    python3 wsserver.py > srv.out 2>srv.err &
    SRVPID=$!
    PORT=""
    for _ in $(seq 1 100); do
        PORT="$(head -1 srv.out 2>/dev/null)"
        [ -n "${PORT}" ] && break
        sleep 0.1
    done
    if [ -z "${PORT}" ]; then
        echo "  FAIL: loopback WebSocket server did not start"
        sed 's/^/      /' srv.err
        FAIL=$((FAIL + 1))
    elif ! build "
m=t;
i=ws:std.ws;
i=io:std.io;
f=main():i64{
  let conn=mt ws.connect(\"ws://127.0.0.1:${PORT}/echo\"){\$ok:c c;\$err:e <1};
  mt ws.send(conn;\"REALSEND\"){\$ok:v io.eprintln(\"ws.send -> \$ok\");\$err:e io.eprintln(\"ws.send -> \$err\")};
  <0
};"; then
        echo "  FAIL: negative control did not compile"
        FAIL=$((FAIL + 1))
        kill "${SRVPID}" 2>/dev/null
    else
        arm="$(./run.bin < /dev/null 2>&1)"
        wait "${SRVPID}" 2>/dev/null
        got="$(sed -n '2p' srv.out)"
        rm -f run.bin
        if [ "${arm}" = 'ws.send -> $ok' ] && [ "${got}" = "REALSEND" ]; then
            echo "  PASS: a genuine connection still sends (server received REALSEND)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: genuine send broken: arm [${arm}], server received [${got}]"
            sed 's/^/      /' srv.err
            FAIL=$((FAIL + 1))
        fi
    fi
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
