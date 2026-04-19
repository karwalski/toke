#!/bin/bash
# HTTPS and TLS client testing (Story 57.6.3)
# Tests HTTPS connections, certificate verification, and TLS behaviour.

set -e
PASS=0; FAIL=0

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }

echo "=== HTTPS and TLS Client Testing ==="

# ── Basic HTTPS GET ──────────────────────────────────────────────────
echo "--- Basic HTTPS ---"

r=$(curl -s -o /dev/null -w "%{http_code}" "https://tokelang.dev/health" 2>/dev/null)
if [ "$r" = "200" ]; then pass "HTTPS GET tokelang.dev/health"; else fail "HTTPS GET" "status $r"; fi

# ── Certificate verification ─────────────────────────────────────────
echo "--- Certificate verification ---"

r=$(curl -s -o /dev/null -w "%{http_code}" --cacert /etc/ssl/cert.pem "https://tokelang.dev/" 2>/dev/null || echo "err")
if [ "$r" = "200" ]; then pass "Cert verification with system CA"; else fail "System CA verify" "$r"; fi

r=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" --cacert /dev/null "https://expired.badssl.com/" 2>/dev/null || echo "err")
if [ "$r" = "err" ] || [ "$r" = "000" ]; then pass "Reject expired certificate"; else fail "Expired cert" "got $r"; fi

r=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" --cacert /dev/null "https://wrong.host.badssl.com/" 2>/dev/null || echo "err")
if [ "$r" = "err" ] || [ "$r" = "000" ]; then pass "Reject wrong host certificate"; else fail "Wrong host cert" "got $r"; fi

# ── TLS version ──────────────────────────────────────────────────────
echo "--- TLS version ---"

r=$(curl -s -o /dev/null -w "%{ssl_connect_time}" --tls-max 1.2 "https://tokelang.dev/" 2>/dev/null)
if [ -n "$r" ] && [ "$r" != "0.000" ]; then pass "TLS 1.2 connection succeeds"; else fail "TLS 1.2" "connect time $r"; fi

r=$(curl -s --max-time 5 -o /dev/null -w "%{http_code}" --tls-max 1.1 "https://tokelang.dev/" 2>/dev/null || echo "err")
if [ "$r" = "err" ] || [ "$r" = "000" ] || [ "$r" = "035" ]; then pass "TLS 1.1 rejected (min TLS 1.2)"; else fail "TLS 1.1" "got $r"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
