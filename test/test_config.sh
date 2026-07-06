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

# Test 6: [capabilities] section (124.4b, ADR-0010) parses into the grant set.
# net=4 | fs_read=1 => cap-grants 0x5, enforce on. Scoped string coarsened to class.
cat > "$TMPDIR/caps.toml" <<'EOF'
[capabilities]
net = true
fs_read = "/srv/www"
fs_write = false
enforce = true
EOF
OUTCAP=$("$TKC" --config="$TMPDIR/caps.toml" --show-limits 2>&1)
if echo "$OUTCAP" | grep -q "cap-grants *= *0x5" \
   && echo "$OUTCAP" | grep -q "cap-present *= *1" \
   && echo "$OUTCAP" | grep -q "cap-enforce *= *1"; then
    echo "  PASS: [capabilities] parses net+fs_read grant, enforce"
else
    echo "  FAIL: [capabilities] parse"
    echo "  GOT: $OUTCAP"
    exit 1
fi

# Test 7: CLI --allow-* unions with (and can stand alone against) config.
OUTCLI=$("$TKC" --allow-net --allow-run --show-limits 2>&1)
if echo "$OUTCLI" | grep -q "cap-grants *= *0x14" \
   && echo "$OUTCLI" | grep -q "cap-present *= *1"; then
    echo "  PASS: --allow-net --allow-run => 0x14 (net|process_spawn)"
else
    echo "  FAIL: CLI --allow-* parse"
    echo "  GOT: $OUTCLI"
    exit 1
fi

# Test 8: no capability config => nothing baked (pure no-op default).
OUTNONE=$(cd /tmp && "$TKC" --show-limits 2>&1)
if echo "$OUTNONE" | grep -q "cap-present *= *0"; then
    echo "  PASS: no capability config => cap-present = 0"
else
    echo "  FAIL: expected cap-present = 0 without config"
    echo "  GOT: $OUTNONE"
    exit 1
fi

echo ""
echo "All config tests passed."
