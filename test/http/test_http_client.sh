#!/bin/bash
# HTTP client basic operations test suite (Story 57.6.2)
# Tests against the local test API server (57.6.1) on port 8099.
#
# Usage:
#   1. Start the test server:  toke test/http/test_api_server.tk &
#   2. Run this script:        bash test/http/test_http_client.sh
#
# Also tests against external APIs (httpbin.org, jsonplaceholder) if
# the --external flag is passed.

set -e
PASS=0; FAIL=0; SKIP=0
BASE="http://localhost:8099"

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP: $1"; }

echo "=== HTTP Client Basic Operations ==="

# ── GET ──────────────────────────────────────────────────────────────
echo "--- GET requests ---"

r=$(curl -s "$BASE/api/ping")
if echo "$r" | grep -q '"status":"ok"'; then pass "GET /api/ping"; else fail "GET /api/ping" "$r"; fi

r=$(curl -s "$BASE/api/echo?msg=hello")
if echo "$r" | grep -q '"echo":"hello"'; then pass "GET /api/echo?msg=hello"; else fail "GET /api/echo" "$r"; fi

r=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/status/204")
if [ "$r" = "204" ]; then pass "GET /api/status/204"; else fail "GET /api/status/204" "got $r"; fi

r=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/status/404")
if [ "$r" = "404" ]; then pass "GET /api/status/404"; else fail "GET /api/status/404" "got $r"; fi

# ── POST ─────────────────────────────────────────────────────────────
echo "--- POST requests ---"

r=$(curl -s -X POST -d '{"key":"value"}' -H "Content-Type: application/json" "$BASE/api/echo")
if echo "$r" | grep -q '"echo"'; then pass "POST /api/echo JSON"; else fail "POST /api/echo JSON" "$r"; fi

r=$(curl -s -X POST -d 'field=test&other=123' "$BASE/api/form")
if echo "$r" | grep -q 'form_data'; then pass "POST /api/form"; else fail "POST /api/form" "$r"; fi

# ── PUT ──────────────────────────────────────────────────────────────
echo "--- PUT requests ---"

r=$(curl -s -X PUT -d '{"update":true}' -H "Content-Type: application/json" "$BASE/api/data")
if echo "$r" | grep -q '"method":"PUT"'; then pass "PUT /api/data"; else fail "PUT /api/data" "$r"; fi

# ── DELETE ───────────────────────────────────────────────────────────
echo "--- DELETE requests ---"

r=$(curl -s -X DELETE "$BASE/api/data")
if echo "$r" | grep -q '"deleted":true'; then pass "DELETE /api/data"; else fail "DELETE /api/data" "$r"; fi

# ── Custom headers ───────────────────────────────────────────────────
echo "--- Custom headers ---"

r=$(curl -s -H "X-Custom: testvalue" "$BASE/api/headers")
if echo "$r" | grep -qi 'testvalue'; then pass "Custom header echo"; else fail "Custom header echo" "$r"; fi

# ── Auth ─────────────────────────────────────────────────────────────
echo "--- Authentication ---"

r=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/auth")
if [ "$r" = "401" ]; then pass "GET /api/auth (no token) returns 401"; else fail "401 without auth" "got $r"; fi

r=$(curl -s -H "Authorization: Bearer test123" "$BASE/api/auth")
if echo "$r" | grep -q 'Bearer test123'; then pass "GET /api/auth (with token)"; else fail "Auth with token" "$r"; fi

# ── Streaming ────────────────────────────────────────────────────────
echo "--- Streaming ---"

r=$(curl -s "$BASE/api/stream" | wc -l | tr -d ' ')
if [ "$r" -ge "5" ]; then pass "GET /api/stream (5+ lines)"; else fail "Stream response" "got $r lines"; fi

# ── External APIs (optional) ─────────────────────────────────────────
if [ "$1" = "--external" ]; then
    echo "--- External APIs ---"

    r=$(curl -s "https://jsonplaceholder.typicode.com/posts/1")
    if echo "$r" | grep -q '"userId"'; then pass "jsonplaceholder GET /posts/1"; else fail "jsonplaceholder" "$r"; fi

    r=$(curl -s -X POST -d '{"title":"test"}' -H "Content-Type: application/json" "https://jsonplaceholder.typicode.com/posts")
    if echo "$r" | grep -q '"id"'; then pass "jsonplaceholder POST /posts"; else fail "jsonplaceholder POST" "$r"; fi
else
    skip "External API tests (pass --external to enable)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
