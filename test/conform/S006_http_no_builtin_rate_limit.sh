#!/usr/bin/env bash
# S006_http_no_builtin_rate_limit.sh — the server does not throttle a
# well-behaved client (story 127.60).
#
# WHAT WAS REMOVED.  std.http carried a per-IP rate limiter: 200 per 60
# seconds as two compile-time constants, a 1024-bucket table, and a check at
# the top of handle_connection() and handle_tls_connection().  There was no
# setter, no environment variable, and no mention of it in http.tki, http.tk
# or http.md.  It was removed rather than tuned, because it was wrong in both
# directions at once.
#
#   IT DID NOT THROTTLE.  The check ran once per CONNECTION, not per request,
#   despite its own comment saying "requests per window per IP" — so a client
#   on one keep-alive socket was never limited (measured: 886 of 900 requests
#   served, no 429), while 199 separate connections from the same address
#   tripped it.  And a bucket was reset whenever the stored address differed
#   from the caller's, so two addresses colliding in 1024 buckets wiped each
#   other's window every time and NEITHER was ever limited: driving the real
#   function, 10.0.1.172 and 10.0.2.78 collide, and 100000 alternating calls
#   each were allowed in full where one address alone stops at 200.
#
#   IT THROTTLED THE WRONG TRAFFIC.  Identity came only from the socket peer;
#   no X-Forwarded-For handling existed.  Behind a proxy or CDN every client
#   shared one bucket and the site was capped at 200 connections a minute.
#
#   AND THE NUMBER WAS NOT 200.  The table was copied into each pre-forked
#   worker, so the cap was 200 x workers, decided by whichever worker
#   accepted — measured at 400 with the two workers http.serve() uses.  h2
#   over TLS skipped the check entirely.
#
# WHAT THIS PINS.  Behaviour, not implementation: one well-behaved client is
# served in full, both over many short connections and over sustained
# keep-alive traffic, and never sees a 429.  The old limiter failed case 1 at
# connection 199 with one worker and at 400 with http.serve's two, so this is
# a real gate on reinstating that shape — while leaving room for a future
# limiter that is actually correct, which would not trip on this traffic
# either.
#
# Rate limiting belongs in front of the server.  See docs/stdlib/http.md,
# "Rate limiting", and the note in src/stdlib/http.c where the table was.
#
# Story: 127.60

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_s006_XXXXXX)"
trap 'rm -rf "${WORK}"; [ -n "${SRV:-}" ] && kill "${SRV}" 2>/dev/null; [ -n "${SRV:-}" ] && pkill -P "${SRV}" 2>/dev/null' EXIT
cd "${WORK}"

echo "S006: the server does not throttle a well-behaved client"
echo "--------------------------------------"

# An ephemeral port, so concurrent runs and leftover listeners cannot collide.
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"

cat > srv.tk <<EOF
m=srv;
i=http:std.http;
f=greet(req:i64):i64{ <http.resok("hi") };
f=main():i64{
  http.gethandler("/api/greet/:name";&greet);
  http.serve(${PORT});
  <0
};
EOF

if ! "${TKC}" --allow-all -o srv.bin srv.tk >build.log 2>&1 || [ ! -x srv.bin ]; then
    echo "  FAIL: server did not build"
    sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

./srv.bin >/dev/null 2>&1 &
SRV=$!
READY=0
for _ in $(seq 1 80); do
    python3 -c "
import socket,sys
s=socket.socket()
s.settimeout(0.3)
sys.exit(0 if s.connect_ex(('127.0.0.1',${PORT}))==0 else 1)" && { READY=1; break; }
    sleep 0.25
done
# 127.156: a server that never came up used to fall through to the probe,
# where 400 refused connections read as "throttled".  Say what happened.
if [ "${READY}" -ne 1 ]; then
    echo "  FAIL: HARNESS — the server never accepted a connection on ${PORT};"
    echo "        nothing below would test throttling, so it is not run."
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

cat > probe.py <<'PYEOF'
import socket, sys, json
port = int(sys.argv[1])
REQ = b"GET /api/greet/world HTTP/1.1\r\nHost: h\r\nConnection: keep-alive\r\n\r\n"

def drain(s):
    buf = b""; want = -1
    while True:
        d = s.recv(65536)
        if not d: return buf
        buf += d
        if want < 0:
            i = buf.find(b"\r\n\r\n")
            if i < 0: continue
            cl = 0
            for line in buf[:i].split(b"\r\n"):
                if line.lower().startswith(b"content-length:"):
                    cl = int(line.split(b":", 1)[1])
            want = i + 4 + cl
        if len(buf) >= want: return buf

def code(r):
    p = r.split(b" ")
    return p[1].decode() if len(p) > 1 else "closed"

# Case 1: 400 short connections.  The old limiter 429'd from connection 199
# with one worker and 400 with http.serve's two.
counts = {}
for _ in range(400):
    try:
        s = socket.create_connection(("127.0.0.1", port)); s.settimeout(15)
        s.sendall(REQ); c = code(drain(s)); s.close()
    except OSError:
        c = "err"
    counts[c] = counts.get(c, 0) + 1

# Case 2: 500 requests over keep-alive connections, reconnecting when the
# server closes one (KEEPALIVE_MAX_REQUESTS, and the worker pool closes them
# far sooner than that).  What is asserted is that 500 requests are SERVED,
# never that one socket carries them: the old limiter counted connections, so
# this is the traffic shape it was blind to and the shape a per-request
# limiter would be the one to trip on.
counts2 = {}
served = 0
s = None
while served < 500:
    if s is None:
        try:
            s = socket.create_connection(("127.0.0.1", port)); s.settimeout(15)
        except OSError:
            counts2["err"] = counts2.get("err", 0) + 1
            break
    try:
        s.sendall(REQ); c = code(drain(s))
    except OSError:
        c = "err"
    if c == "200":
        served += 1
    counts2[c] = counts2.get(c, 0) + 1
    if c != "200":
        try: s.close()
        except OSError: pass
        s = None
        if c not in ("closed",): break
if s: s.close()

print(json.dumps({"short_connections": counts, "one_connection": counts2}))
PYEOF

if ! python3 probe.py "${PORT}" > probe.out 2>probe.err; then
    echo "  FAIL: probe did not run"
    sed 's/^/      /' probe.err | head -6
    FAIL=$((FAIL + 1))
else
    RESULT="$(cat probe.out)"
    echo "  ${RESULT}"
    if python3 -c "
import json,sys
d=json.load(open('probe.out'))
sys.exit(0 if d['short_connections'].get('200',0)==400 else 1)"; then
        echo "  PASS: 400 short connections from one client are all served"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: a well-behaved client was throttled over 400 short connections"
        FAIL=$((FAIL + 1))
    fi
    if python3 -c "
import json,sys
d=json.load(open('probe.out'))
sys.exit(0 if d['one_connection'].get('200',0)==500 and d['one_connection'].get('429',0)==0 else 1)"; then
        echo "  PASS: 500 keep-alive requests are all served, none throttled"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: keep-alive requests were throttled or not all served"
        FAIL=$((FAIL + 1))
    fi
fi

kill "${SRV}" 2>/dev/null
pkill -P "${SRV}" 2>/dev/null

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
