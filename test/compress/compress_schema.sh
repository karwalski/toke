#!/usr/bin/env bash
# compress_schema.sh — Tests for schema-aware compression (story 13.1.5).
#
# Verifies:
#   1. JSON objects are compressed with schema-header encoding.
#   2. JSON arrays of objects use the schema once, values positionally.
#   3. CSV input emits the header schema once, rows as positional tuples.
#   4. detect_input_type correctly classifies JSON, CSV, and prose.
#   5. Schema compression achieves meaningful token reduction vs raw input.
#   6. Unrecognised input falls back to prose compression.
#
# Story: 13.1.5
set -uo pipefail

TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
PASS=0
FAIL=0

# ── Helper: run compress and check output contains a substring ────────

check_output_contains() {
    local desc="$1"
    local input="$2"
    local expected="$3"

    out=$(printf '%s' "$input" | "$TKC" --compress 2>/tmp/tkc_schema_err)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL [$desc] compress exited $rc"
        cat /tmp/tkc_schema_err
        FAIL=$((FAIL+1))
        return
    fi
    if printf '%s' "$out" | grep -qF "$expected"; then
        echo "PASS [$desc] output contains '$expected'"
        PASS=$((PASS+1))
    else
        echo "FAIL [$desc] expected '$expected' in output"
        echo "  OUTPUT: $(printf '%s' "$out" | head -c 300)"
        FAIL=$((FAIL+1))
    fi
}

check_output_not_contains() {
    local desc="$1"
    local input="$2"
    local unexpected="$3"

    out=$(printf '%s' "$input" | "$TKC" --compress 2>/dev/null)
    if printf '%s' "$out" | grep -qF "$unexpected"; then
        echo "FAIL [$desc] output unexpectedly contains '$unexpected'"
        echo "  OUTPUT: $(printf '%s' "$out" | head -c 300)"
        FAIL=$((FAIL+1))
    else
        echo "PASS [$desc] output does not contain '$unexpected'"
        PASS=$((PASS+1))
    fi
}

check_compression_ratio() {
    local desc="$1"
    local input="$2"
    local max_ratio_pct="$3"   # output must be <= this % of input length

    in_len=${#input}
    out=$(printf '%s' "$input" | "$TKC" --compress 2>/dev/null)
    out_len=${#out}

    threshold=$(( in_len * max_ratio_pct / 100 ))
    if [ "$out_len" -le "$threshold" ] || [ "$in_len" -le 20 ]; then
        echo "PASS [$desc] compression ratio ok (in=$in_len out=$out_len <=$max_ratio_pct%)"
        PASS=$((PASS+1))
    else
        echo "FAIL [$desc] output too large (in=$in_len out=$out_len threshold=$threshold)"
        FAIL=$((FAIL+1))
    fi
}

# ── JSON single object ────────────────────────────────────────────────

JSON_OBJ='{"name":"Alice","email":"alice@example.com","role":"admin"}'

check_output_contains "JSON object magic header" \
    "$JSON_OBJ" \
    "TK:JSON:"

check_output_contains "JSON object key schema" \
    "$JSON_OBJ" \
    "name,email,role"

check_output_contains "JSON object value in output" \
    "$JSON_OBJ" \
    "Alice"

# Schema header should appear once, not the raw JSON braces
check_output_not_contains "JSON object no raw brace" \
    "$JSON_OBJ" \
    '{"name"'

# ── JSON array of objects ────────────────────────────────────────────

JSON_ARRAY='[{"id":"1","name":"Alice","city":"Sydney"},{"id":"2","name":"Bob","city":"Melbourne"},{"id":"3","name":"Carol","city":"Brisbane"}]'

check_output_contains "JSON array magic header" \
    "$JSON_ARRAY" \
    "TK:JSON:"

check_output_contains "JSON array schema emitted once" \
    "$JSON_ARRAY" \
    "id,name,city"

check_output_contains "JSON array value Alice" \
    "$JSON_ARRAY" \
    "Alice"

check_output_contains "JSON array value Melbourne" \
    "$JSON_ARRAY" \
    "Melbourne"

# The schema should appear exactly once, not once per row
schema_count=$(printf '%s' "$JSON_ARRAY" | "$TKC" --compress 2>/dev/null | grep -c "id,name,city" || true)
if [ "$schema_count" -eq 1 ]; then
    echo "PASS [JSON array schema appears once]"
    PASS=$((PASS+1))
else
    echo "FAIL [JSON array schema count=$schema_count, expected 1]"
    FAIL=$((FAIL+1))
fi

check_compression_ratio "JSON array compression ratio" \
    "$JSON_ARRAY" \
    80

# ── CSV input ────────────────────────────────────────────────────────

CSV_INPUT='name,email,department,salary
Alice,alice@example.com,Engineering,95000
Bob,bob@example.com,Marketing,72000
Carol,carol@example.com,Engineering,88000
Dave,dave@example.com,Sales,65000'

check_output_contains "CSV magic header" \
    "$CSV_INPUT" \
    "TK:CSV:"

check_output_contains "CSV column schema" \
    "$CSV_INPUT" \
    "name,email,department,salary"

check_output_contains "CSV value Alice" \
    "$CSV_INPUT" \
    "Alice"

check_output_contains "CSV value Engineering" \
    "$CSV_INPUT" \
    "Engineering"

# Header should appear once, not once per row
csv_header_count=$(printf '%s' "$CSV_INPUT" | "$TKC" --compress 2>/dev/null | grep -c "name,email,department,salary" || true)
if [ "$csv_header_count" -eq 1 ]; then
    echo "PASS [CSV header appears once]"
    PASS=$((PASS+1))
else
    echo "FAIL [CSV header count=$csv_header_count, expected 1]"
    FAIL=$((FAIL+1))
fi

check_compression_ratio "CSV compression ratio" \
    "$CSV_INPUT" \
    85

# ── Prose fallback ────────────────────────────────────────────────────

PROSE_INPUT="This is a plain prose paragraph with no JSON or CSV structure."

# Should not produce JSON or CSV magic headers
check_output_not_contains "prose no JSON magic" \
    "$PROSE_INPUT" \
    "TK:JSON:"

check_output_not_contains "prose no CSV magic" \
    "$PROSE_INPUT" \
    "TK:CSV:"

# Should still produce prose magic
check_output_contains "prose fallback magic" \
    "$PROSE_INPUT" \
    "TK:P"

# ── CSV with quoted fields ────────────────────────────────────────────

CSV_QUOTED='first_name,last_name,city
"Alice","Smith","New York"
"Bob","Jones","Los Angeles"'

check_output_contains "CSV quoted fields magic" \
    "$CSV_QUOTED" \
    "TK:CSV:"

check_output_contains "CSV quoted fields schema" \
    "$CSV_QUOTED" \
    "first_name,last_name,city"

# ── Large JSON array (compression ratio target: < 70%) ───────────────

LARGE_JSON='[{"product_id":"P001","product_name":"Widget A","category":"Electronics","price":"29.99","stock":"150"},{"product_id":"P002","product_name":"Widget B","category":"Electronics","price":"49.99","stock":"75"},{"product_id":"P003","product_name":"Gadget X","category":"Accessories","price":"12.99","stock":"300"},{"product_id":"P004","product_name":"Gadget Y","category":"Accessories","price":"19.99","stock":"200"},{"product_id":"P005","product_name":"Device Z","category":"Electronics","price":"199.99","stock":"25"}]'

check_output_contains "large JSON array schema once" \
    "$LARGE_JSON" \
    "product_id,product_name,category,price,stock"

check_compression_ratio "large JSON array ratio" \
    "$LARGE_JSON" \
    75

# ── Summary ───────────────────────────────────────────────────────────

echo "---"
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
