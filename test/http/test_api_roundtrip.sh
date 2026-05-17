#!/bin/bash
# HTTP API roundtrip test (Story 88.3.1-88.3.2)
# Compiles a small toke HTTP server, starts it, tests endpoints, cleans up.

set -e

TOKE="/Users/matthew.watt/tk/toke/toke"
SERVER_SRC="/tmp/toke_test_server.tk"
SERVER_BIN="/tmp/toke_test_server"
PORT=18923
PASS=0; FAIL=0

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  rm -f "$SERVER_SRC" "$SERVER_BIN"
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
  io.println("Server listening on :18923");
  http.serve(18923);
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
"$SERVER_BIN" &
SERVER_PID=$!

# 4. Wait for startup
sleep 2

# 5. Test /api/test endpoint
echo "--- Testing /api/test ---"
r=$(curl -s "http://localhost:$PORT/api/test")
if echo "$r" | grep -q '"status":"ok"'; then
  pass "GET /api/test returns status ok"
else
  fail "GET /api/test" "got: $r"
fi

# 6. Test 404 for unknown route
echo "--- Testing 404 ---"
code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/nonexistent")
if [ "$code" = "404" ]; then
  pass "GET /nonexistent returns 404"
else
  fail "GET /nonexistent" "expected 404, got $code"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
