#!/bin/bash
# SOAP and XML payload testing (Story 57.6.6)
# Tests XML request/response handling against the local test API.

set -e
PASS=0; FAIL=0
BASE="http://localhost:8099"

pass() { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1 — $2"; }

echo "=== SOAP/XML Payload Testing ==="

# ── XML POST ────────────────────────────────────────────────────────
echo "--- XML request ---"

SOAP_REQ='<?xml version="1.0"?><soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/"><soap:Body><GetStatus/></soap:Body></soap:Envelope>'

r=$(curl -s -X POST -H "Content-Type: text/xml" -d "$SOAP_REQ" "$BASE/api/echo")
if echo "$r" | grep -q "echo"; then pass "XML/SOAP payload accepted"; else fail "SOAP POST" "$r"; fi

# ── XML content type ────────────────────────────────────────────────
echo "--- XML content type ---"

r=$(curl -s -X POST -H "Content-Type: application/xml" -d "<request><id>42</id></request>" "$BASE/api/echo")
if echo "$r" | grep -q "echo"; then pass "application/xml content type"; else fail "XML content type" "$r"; fi

# ── SOAP action header ──────────────────────────────────────────────
echo "--- SOAPAction header ---"

r=$(curl -s -H "SOAPAction: GetStatus" -H "Content-Type: text/xml" "$BASE/api/headers")
if echo "$r" | grep -qi "soapaction"; then pass "SOAPAction header forwarded"; else fail "SOAPAction" "$r"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
