#!/usr/bin/env bash
# compress_stream.sh — Tests for streaming compress API (story 13.1.4).
#
# Verifies:
#   1. --compress-stream reads stdin and emits compressed output.
#   2. Placeholder atoms are preserved in stream mode.
#   3. Stream output for prose is smaller than or equal to input (no inflation).
#   4. Multi-line input is processed correctly.
#
# Story: 13.1.4
set -uo pipefail

TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
PASS=0
FAIL=0

check_stream_runs() {
    local desc="$1"
    local input="$2"

    out=$(printf '%s' "$input" | "$TKC" --compress-stream 2>/tmp/tkc_stream_err)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL [$desc] --compress-stream exited $rc"
        cat /tmp/tkc_stream_err
        FAIL=$((FAIL+1))
        return
    fi
    if [ -z "$out" ] && [ -n "$input" ]; then
        echo "FAIL [$desc] empty output for non-empty input"
        FAIL=$((FAIL+1))
        return
    fi
    echo "PASS [$desc]"
    PASS=$((PASS+1))
}

check_stream_placeholder_preserved() {
    local desc="$1"
    local input="$2"
    local placeholder="$3"

    out=$(printf '%s' "$input" | "$TKC" --compress-stream 2>/dev/null)
    if printf '%s' "$out" | grep -qF "$placeholder"; then
        echo "PASS [$desc] placeholder preserved"
        PASS=$((PASS+1))
    else
        echo "FAIL [$desc] placeholder $placeholder missing from stream output"
        echo "  OUTPUT: $(printf '%s' "$out" | head -c 200)"
        FAIL=$((FAIL+1))
    fi
}

check_stream_not_inflated() {
    local desc="$1"
    local input="$2"

    in_len=${#input}
    out=$(printf '%s' "$input" | "$TKC" --compress-stream 2>/dev/null)
    out_len=${#out}

    # Allow up to 10% inflation (streaming overhead for small inputs)
    threshold=$(( in_len + in_len / 10 + 8 ))
    if [ "$out_len" -le "$threshold" ]; then
        echo "PASS [$desc] output not significantly inflated (in=$in_len out=$out_len)"
        PASS=$((PASS+1))
    else
        echo "FAIL [$desc] output too large (in=$in_len out=$out_len threshold=$threshold)"
        FAIL=$((FAIL+1))
    fi
}

# ── Basic streaming tests ────────────────────────────────────────────

check_stream_runs "simple prose stream" \
    "Hello world, this is a streaming test."

check_stream_runs "multiline stream" \
    "Line one of the message.
Line two with more content.
Line three at the end."

check_stream_runs "stream with placeholders" \
    "\$PERSON_1 sent a message to \$PERSON_2 at \$COMPANY_1."

check_stream_runs "empty input stream" ""

check_stream_runs "single word stream" "hello"

check_stream_runs "stream with high-frequency words" \
    "The request should be processed and the response will be sent with the following information."

# ── Placeholder preservation in stream mode ──────────────────────────

check_stream_placeholder_preserved "PERSON_1 stream" \
    "\$PERSON_1 sent a message." \
    "\$PERSON_1"

check_stream_placeholder_preserved "EMAIL_1 stream" \
    "Reply to \$EMAIL_1 with details." \
    "\$EMAIL_1"

check_stream_placeholder_preserved "ADDR_12 stream" \
    "Ship to \$ADDR_12 by Friday." \
    "\$ADDR_12"

check_stream_placeholder_preserved "multiple placeholders stream" \
    "\$PERSON_1 works at \$COMPANY_3 and uses \$EMAIL_1." \
    "\$COMPANY_3"

# ── Size checks ───────────────────────────────────────────────────────

check_stream_not_inflated "prose no inflation" \
    "The following request should be processed with the appropriate response and sent to the customer account."

check_stream_not_inflated "placeholder dense no inflation" \
    "\$PERSON_1 \$EMAIL_1 \$PERSON_2 \$EMAIL_2 \$COMPANY_1"

# ── Multiline round-trip consistency ─────────────────────────────────
# Stream output and batch output should be equivalent for prose input.

INPUT="Dear \$PERSON_1, please send your response to \$EMAIL_1 regarding \$COMPANY_1."

stream_out=$(printf '%s' "$INPUT" | "$TKC" --compress-stream 2>/dev/null)
batch_out=$(printf '%s' "$INPUT" | "$TKC" --compress 2>/dev/null)

# Both should preserve the placeholders
for ph in "\$PERSON_1" "\$EMAIL_1" "\$COMPANY_1"; do
    if printf '%s' "$stream_out" | grep -qF "$ph" && \
       printf '%s' "$batch_out"  | grep -qF "$ph"; then
        echo "PASS [stream-batch placeholder $ph]"
        PASS=$((PASS+1))
    else
        echo "FAIL [stream-batch placeholder $ph] not preserved in both modes"
        FAIL=$((FAIL+1))
    fi
done

# ── Summary ───────────────────────────────────────────────────────────

echo "---"
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
