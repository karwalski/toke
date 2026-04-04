#!/bin/bash
# test_config.sh — verify tkc.toml config file support (Story 10.11.8)
set -euo pipefail

TKC="$(cd "$(dirname "$0")/.." && pwd)/tkc"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "=== tkc.toml config tests ==="

# Test 1: Config file overrides defaults
cat > "$TMPDIR/tkc.toml" <<'EOF'
# Test config
max_funcs = 256
max_locals = 512
max_params = 32
max_struct_types = 128
max_imports = 64
arena_block_size = 131072
EOF

cd "$TMPDIR"
OUTPUT=$("$TKC" --show-limits 2>&1)

check() {
    local key="$1" expect="$2"
    if echo "$OUTPUT" | grep -q "$key *= *$expect"; then
        echo "  PASS: $key = $expect"
    else
        echo "  FAIL: expected $key = $expect"
        echo "  GOT: $OUTPUT"
        exit 1
    fi
}

check "max-funcs"   256
check "max-locals"  512
check "max-params"  32
check "max-structs" 128
check "max-imports" 64
check "arena-block" 131072

# Test 2: CLI flags override config
OUTPUT2=$("$TKC" --max-funcs=999 --show-limits 2>&1)
if echo "$OUTPUT2" | grep -q "max-funcs *= *999"; then
    echo "  PASS: CLI --max-funcs=999 overrides config"
else
    echo "  FAIL: CLI override did not work"
    echo "  GOT: $OUTPUT2"
    exit 1
fi

# Test 3: Missing config file is not an error (use defaults)
cd /tmp
OUTPUT3=$("$TKC" --show-limits 2>&1)
if echo "$OUTPUT3" | grep -q "max-funcs *= *128"; then
    echo "  PASS: missing tkc.toml uses defaults"
else
    echo "  FAIL: expected defaults without config"
    echo "  GOT: $OUTPUT3"
    exit 1
fi

# Test 4: --config=PATH works with explicit path
OUTPUT4=$("$TKC" --config="$TMPDIR/tkc.toml" --show-limits 2>&1)
if echo "$OUTPUT4" | grep -q "max-funcs *= *256"; then
    echo "  PASS: --config=PATH loads alternate file"
else
    echo "  FAIL: --config=PATH did not work"
    echo "  GOT: $OUTPUT4"
    exit 1
fi

# Test 5: --config= with nonexistent file is an error
if "$TKC" --config=/nonexistent/tkc.toml --show-limits 2>/dev/null; then
    echo "  FAIL: expected error for missing explicit config"
    exit 1
else
    echo "  PASS: missing explicit config file is an error"
fi

echo ""
echo "All config tests passed."
