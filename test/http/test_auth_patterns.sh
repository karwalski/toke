#!/bin/bash
# Authentication pattern testing (Story 57.6.4)
# Tests Bearer tokens, Basic auth, API keys against local test API (port 8099).

set -e
PASS=0; FAIL=0
BASE="http://localhost:8099"

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }

echo "=== Authentication Pattern Testing ==="

# ── Bearer token ─────────────────────────────────────────────────────
echo "--- Bearer tokens ---"

r=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/api/auth")
if [ "$r" = "401" ]; then pass "No auth returns 401"; else fail "Missing auth" "got $r"; fi

r=$(curl -s -H "Authorization: Bearer tok_live_abc123" "$BASE/api/auth")
if echo "$r" | grep -q "Bearer tok_live_abc123"; then pass "Bearer token accepted"; else fail "Bearer token" "$r"; fi

# ── Basic auth ───────────────────────────────────────────────────────
echo "--- Basic auth ---"

r=$(curl -s -H "Authorization: Basic dXNlcjpwYXNz" "$BASE/api/auth")
if echo "$r" | grep -q "Basic"; then pass "Basic auth header forwarded"; else fail "Basic auth" "$r"; fi

# ── API key in header ────────────────────────────────────────────────
echo "--- API key (header) ---"

r=$(curl -s -H "X-API-Key: sk_test_12345" "$BASE/api/headers")
if echo "$r" | grep -qi "sk_test_12345"; then pass "API key in X-API-Key header"; else fail "API key header" "$r"; fi

# ── API key in query ─────────────────────────────────────────────────
echo "--- API key (query) ---"

r=$(curl -s "$BASE/api/echo?msg=test&api_key=sk_test_99")
if echo "$r" | grep -q '"echo":"test"'; then pass "API key in query string"; else fail "API key query" "$r"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
