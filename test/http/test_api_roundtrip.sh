#!/bin/bash
# HTTP API roundtrip test (Story 88.3.1-88.3.2)
# Compiles a small toke HTTP server, starts it, tests endpoints, cleans up.

set -e

# 127.130: default to the main checkout, as before, but overridable.  This was
# a hard absolute path, so running from a git worktree silently measured the
# MAIN checkout's compiler and reported a pass for a binary the branch had
# never built.  Unset, the behaviour below is byte-identical to before.
TOKE_ROOT=${TOKE_ROOT:-/Users/matthew.watt/tk/toke}
TOKE=${TOKE:-$TOKE_ROOT/toke}
# 135.26 item 4: a private work dir and a kernel-chosen port.  This used a
# fixed port (18923) and fixed /tmp paths, so a second run, or anything else
# holding 18923, made it fail -- or pass against someone else's server.  The
# server now binds port 0 and the port is read back from the
# `DIAGNOSTIC: kind=server_listening port=N` line http.serve prints (135.24).
WORK=$(mktemp -d "${TMPDIR:-/tmp}/toke_api_roundtrip_XXXXXX")
SERVER_SRC="$WORK/server.tk"
SERVER_BIN="$WORK/server"
PORT=""
PASS=0; FAIL=0

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

echo "=== HTTP API Roundtrip Test ==="

# 1. Write the server source
cat > "$SERVER_SRC" << 'TKEOF'
m=test;
i=http:std.http;
i=io:std.io;

f=handler(req:$req):$res{
  <http.jsonresp(200; "{\"status\":\"ok\"}")
};

f=main():i64{
  http.get("/api/test"; &handler);
  http.serve(0);
  <0
};
TKEOF

# 2. Compile
echo "Compiling server..."
"$TOKE" "$SERVER_SRC" --out "$SERVER_BIN"
if [ $? -ne 0 ]; then
  echo "FAIL: compilation failed"
  exit 1
fi

# 3. Start server in background
# --allow-net: without it the server stops at CAP001 before it binds, and
# before 135.26 this script then waited on curl against whatever held 18923.
"$SERVER_BIN" --allow-net 2> "$WORK/server.err" &
SERVER_PID=$!

# 4. Wait for startup: the server says which port it got
for _ in $(seq 1 50); do
  PORT=$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9]*\)$/\1/p' "$WORK/server.err" | head -1)
  [ -n "$PORT" ] && break
  sleep 0.1
done
if [ -z "$PORT" ]; then
  echo "FAIL: the server reported no port: $(head -c 300 "$WORK/server.err")"
  exit 1
fi
echo "Server listening on :$PORT"

# 5. Test /api/test endpoint
echo "--- Testing /api/test ---"
r=$(curl -s --max-time 10 "http://localhost:$PORT/api/test")
if echo "$r" | grep -q '"status":"ok"'; then
  pass "GET /api/test returns status ok"
else
  fail "GET /api/test" "got: $r"
fi

# 6. Test 404 for unknown route
echo "--- Testing 404 ---"
code=$(curl -s --max-time 10 -o /dev/null -w "%{http_code}" "http://localhost:$PORT/nonexistent")
if [ "$code" = "404" ]; then
  pass "GET /nonexistent returns 404"
else
  fail "GET /nonexistent" "expected 404, got $code"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
