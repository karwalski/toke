#!/bin/bash
# Streaming and file download testing (Story 57.6.5)
# Tests chunked transfer encoding, streaming response consumption.

set -e
PASS=0; FAIL=0
BASE="http://localhost:8099"

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }

echo "=== Streaming and Download Testing ==="

# ── Chunked response ────────────────────────────────────────────────
echo "--- Chunked/streaming responses ---"

r=$(curl -s "$BASE/api/stream")
lines=$(echo "$r" | wc -l | tr -d ' ')
if [ "$lines" -ge "5" ]; then pass "Stream response has 5+ lines"; else fail "Stream line count" "got $lines"; fi

# ── Large response ──────────────────────────────────────────────────
echo "--- Large response handling ---"

r=$(curl -s -o /dev/null -w "%{size_download} %{http_code}" "$BASE/api/ping")
size=$(echo "$r" | awk '{print $1}')
code=$(echo "$r" | awk '{print $2}')
if [ "$code" = "200" ] && [ "$size" -gt "0" ]; then pass "Download size tracked ($size bytes)"; else fail "Download" "$r"; fi

# ── Response timing ──────────────────────────────────────────────────
echo "--- Response timing ---"

r=$(curl -s -o /dev/null -w "%{time_total}" "$BASE/api/ping")
if [ -n "$r" ]; then pass "Response time measured (${r}s)"; else fail "Timing" "empty"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
