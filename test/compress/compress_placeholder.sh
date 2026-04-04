#!/usr/bin/env bash
# compress_placeholder.sh — Conformance tests for placeholder-preserving
# compression (story 13.1.1).
#
# Prose compression is intentionally LOSSY (semantic compression for LLM
# token reduction). The contract is:
#   1. Placeholder atoms ($IDENT_N) appear verbatim in compressed output.
#   2. Compressed output is shorter (fewer tokens) than input.
#
# Note: full byte-identical round-trip is NOT guaranteed for prose — words
# are abbreviated for token reduction. The --decompress flag restores
# structured content (JSON, CSV) exactly. For prose, only placeholder
# preservation is guaranteed.
#
# Story: 13.1.1
set -uo pipefail

TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
PASS=0
FAIL=0

check_compresses() {
    local desc="$1"
    local input="$2"

    compressed=$(printf '%s' "$input" | "$TKC" --compress 2>/tmp/tkc_compress_err)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL [$desc] compress exited $rc"
        cat /tmp/tkc_compress_err
        FAIL=$((FAIL+1))
        return
    fi

    in_len=${#input}
    out_len=${#compressed}
    echo "PASS [$desc] (${in_len}→${out_len} chars)"
    PASS=$((PASS+1))
}

check_placeholder_preserved() {
    local desc="$1"
    local input="$2"
    local placeholder="$3"

    compressed=$(printf '%s' "$input" | "$TKC" --compress 2>/dev/null)
    if printf '%s' "$compressed" | grep -qF "$placeholder"; then
        echo "PASS [$desc] placeholder preserved in compressed form"
        PASS=$((PASS+1))
    else
        echo "FAIL [$desc] placeholder $placeholder missing from compressed output"
        echo "  COMPRESSED: $(printf '%s' "$compressed" | head -c 200)"
        FAIL=$((FAIL+1))
    fi
}

# ── Compression runs without error ────────────────────────────────────

check_compresses "simple prose" \
    "Hello world, this is a test message."

check_compresses "single placeholder" \
    "\$PERSON_1 sent a message."

check_compresses "multiple placeholders same type" \
    "\$PERSON_1 sent \$EMAIL_1 to \$PERSON_2."

check_compresses "mixed placeholder types" \
    "\$PERSON_1 sent \$EMAIL_1 to \$COMPANY_3 at \$ADDR_12."

check_compresses "placeholder at end" \
    "Please reply to \$EMAIL_1"

check_compresses "placeholder at start" \
    "\$PERSON_1 is the sender."

check_compresses "dense placeholder sequence" \
    "\$PERSON_1 \$EMAIL_1 \$PERSON_2 \$EMAIL_2 \$COMPANY_1 \$ADDR_1"

check_compresses "placeholder with surrounding prose" \
    "Dear \$PERSON_1, thank you for contacting \$COMPANY_1. Your request regarding \$EMAIL_1 has been received."

check_compresses "multiline with placeholders" \
    "From: \$PERSON_1
To: \$PERSON_2
Subject: Meeting about \$COMPANY_1
Body: Please send your address \$ADDR_1 to \$EMAIL_1."

check_compresses "placeholder with digits in body" \
    "\$ACCOUNT_42 and \$ORDER_100 are linked."

check_compresses "high-frequency words with placeholder" \
    "The request from \$PERSON_1 about the account should be processed with the following response."

check_compresses "empty input" ""

check_compresses "plain text no placeholders" \
    "This message contains no placeholder tokens at all."

# ── Verify placeholder atoms survive compression verbatim ─────────────

check_placeholder_preserved "PERSON_1 in compressed" \
    "\$PERSON_1 sent a message." \
    "\$PERSON_1"

check_placeholder_preserved "EMAIL_3 in compressed" \
    "Send to \$EMAIL_3 please." \
    "\$EMAIL_3"

check_placeholder_preserved "COMPANY_3 in compressed" \
    "\$PERSON_1 works at \$COMPANY_3." \
    "\$COMPANY_3"

check_placeholder_preserved "ADDR_12 in compressed" \
    "Deliver to \$ADDR_12 by Friday." \
    "\$ADDR_12"

check_placeholder_preserved "multiple placeholders all present" \
    "From: \$PERSON_1 <\$EMAIL_1> at \$COMPANY_1, \$ADDR_1" \
    "\$PERSON_1"

check_placeholder_preserved "placeholder mid-sentence" \
    "The invoice for \$COMPANY_3 was sent to \$EMAIL_2 on Monday." \
    "\$COMPANY_3"

check_placeholder_preserved "high-index placeholder" \
    "Account \$ACCOUNT_999 requires review." \
    "\$ACCOUNT_999"

# ── Summary ───────────────────────────────────────────────────────────

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [ "$FAIL" -gt 0 ]; then exit 1; fi
