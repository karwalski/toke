#!/bin/bash
# Performance baseline test (Story 89.3.3)
# Starts the ooke server, runs sequential GET requests, measures throughput.

set -e

OOKE_DIR="/Users/matthew.watt/tk/toke-ooke"
OOKE_BIN="$OOKE_DIR/ooke-toke"
PORT=3000
BASE="http://localhost:$PORT"
REQUESTS=100
PASS=0; FAIL=0

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }

cleanup() {
  if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "=== Performance Baseline Test ==="

# --- Pre-checks ---
if [ ! -x "$OOKE_BIN" ]; then
  fail "ooke binary" "$OOKE_BIN not found or not executable"
  echo "Results: $PASS passed, $FAIL failed"
  exit 1
fi

# --- Start server ---
cd "$OOKE_DIR"
"$OOKE_BIN" serve > /dev/null 2>&1 &
SERVER_PID=$!
sleep 2

# Verify server is up
if ! curl -s "$BASE/api/health" > /dev/null 2>&1; then
  fail "server start" "ooke server did not respond on port $PORT"
  echo "Results: $PASS passed, $FAIL failed"
  exit 1
fi
pass "server started on port $PORT"

echo ""
echo "--- Benchmark: $REQUESTS sequential requests ---"

# --- Benchmark /api/health ---
START=$(date +%s%N)
for i in $(seq 1 $REQUESTS); do
  curl -s "$BASE/api/health" > /dev/null
done
END=$(date +%s%N)
HEALTH_MS=$(( (END - START) / 1000000 ))

if [ "$HEALTH_MS" -gt 0 ]; then
  HEALTH_RPS=$(( REQUESTS * 1000 / HEALTH_MS ))
  pass "API /api/health: $REQUESTS requests in ${HEALTH_MS}ms ($HEALTH_RPS req/s)"
else
  fail "API /api/health" "elapsed time was 0ms (measurement error)"
fi

# --- Benchmark /about (static page) ---
START=$(date +%s%N)
for i in $(seq 1 $REQUESTS); do
  curl -s "$BASE/about" > /dev/null
done
END=$(date +%s%N)
ABOUT_MS=$(( (END - START) / 1000000 ))

if [ "$ABOUT_MS" -gt 0 ]; then
  ABOUT_RPS=$(( REQUESTS * 1000 / ABOUT_MS ))
  pass "Static /about: $REQUESTS requests in ${ABOUT_MS}ms ($ABOUT_RPS req/s)"
else
  fail "Static /about" "elapsed time was 0ms (measurement error)"
fi

# --- Summary ---
echo ""
echo "=== Performance Baseline Results ==="
echo "  /api/health : ${HEALTH_MS}ms total, ~${HEALTH_RPS:-0} req/s"
echo "  /about      : ${ABOUT_MS}ms total, ~${ABOUT_RPS:-0} req/s"
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
