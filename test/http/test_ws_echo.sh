#!/bin/bash
# WebSocket echo test stub (Story 88.3.5)
# Tests WS connectivity using wscat or websocat.
# Currently a stub — requires a WS echo server to run full tests.
# The toke std.ws module provides WebSocket support.

set -e

PASS=0; FAIL=0; SKIP=0

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP: $1"; }

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "=== WebSocket Echo Test ==="

# --- Check for WS client tool ---
WS_TOOL=""

if command -v wscat > /dev/null 2>&1; then
  WS_TOOL="wscat"
  pass "wscat found at $(command -v wscat)"
elif command -v websocat > /dev/null 2>&1; then
  WS_TOOL="websocat"
  pass "websocat found at $(command -v websocat)"
else
  skip "No WebSocket client found"
  echo ""
  echo "  To run WebSocket tests, install one of:"
  echo "    npm install -g wscat"
  echo "    brew install websocat"
  echo ""
  echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
  exit 0
fi

# --- WS echo test (requires a running WS echo server) ---
# When a toke WS echo server is available, the test below can be enabled.
# Expected usage with std.ws:
#
#   m=ws_echo;
#   i=ws:std.ws;
#   i=io:std.io;
#
#   f=on_msg(conn:$ws_conn; msg:str):void{
#     ws.send(conn; msg)
#   };
#
#   f=main():i64{
#     ws.on_message(&on_msg);
#     io.println("WS echo on :8765");
#     ws.serve(8765)
#   };

WS_PORT=8765
WS_URL="ws://localhost:$WS_PORT"

# Check if anything is listening on the WS port
if ! curl -s -o /dev/null --max-time 1 "http://localhost:$WS_PORT" 2>/dev/null; then
  skip "No WS server running on port $WS_PORT (echo test requires a server)"
  echo ""
  echo "  To run the full echo test:"
  echo "    1. Start a toke WS echo server on port $WS_PORT"
  echo "    2. Re-run this test"
  echo ""
  echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
  exit 0
fi

# Server is running — attempt echo test
echo "  Attempting echo test against $WS_URL ..."

if [ "$WS_TOOL" = "wscat" ]; then
  RESPONSE=$(echo "hello toke" | wscat -c "$WS_URL" --wait 2 2>/dev/null | head -1)
elif [ "$WS_TOOL" = "websocat" ]; then
  RESPONSE=$(echo "hello toke" | websocat --one-message "$WS_URL" 2>/dev/null)
fi

if [ "$RESPONSE" = "hello toke" ]; then
  pass "WS echo returned expected payload"
else
  fail "WS echo" "expected 'hello toke', got '$RESPONSE'"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
