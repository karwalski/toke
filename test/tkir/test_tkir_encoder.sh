#!/usr/bin/env bash
# test_tkir_encoder.sh — Verify that --emit-tkir produces a valid .tkir file.
# Story: 76.1.6a

set -euo pipefail

TKC="${1:-./tkc}"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# --- Test 1: Magic bytes ---
# Write a minimal toke source with a function + arithmetic + return.
cat > "$TMPDIR/add.tk" <<'EOF'
m=test;
f=main():i64{
  <1+2
};
EOF

"$TKC" --emit-tkir "$TMPDIR/add.tk" --out "$TMPDIR/add.tkir"

# Verify the file exists and starts with "TKIR" magic bytes.
if [ ! -f "$TMPDIR/add.tkir" ]; then
    echo "FAIL: add.tkir was not created"
    exit 1
fi

# Read first 4 bytes and check they're "TKIR"
MAGIC=$(head -c 4 "$TMPDIR/add.tkir")
if [ "$MAGIC" != "TKIR" ]; then
    echo "FAIL: magic bytes are '$MAGIC', expected 'TKIR'"
    exit 1
fi

echo "  PASS: test_magic_bytes"

# --- Test 2: File size ---
# A programme with one function should produce a non-trivial .tkir file
# (header + 6 sections, at minimum ~30 bytes).
SIZE=$(wc -c < "$TMPDIR/add.tkir" | tr -d ' ')
if [ "$SIZE" -lt 30 ]; then
    echo "FAIL: file too small ($SIZE bytes)"
    exit 1
fi
echo "  PASS: test_file_size ($SIZE bytes)"

# --- Test 3: String literal in data section ---
cat > "$TMPDIR/hello.tk" <<'EOF'
m=test;
f=main():i64{
  let s="hello";
  <0
};
EOF

"$TKC" --emit-tkir "$TMPDIR/hello.tk" --out "$TMPDIR/hello.tkir"

# The string "hello" should appear somewhere in the binary.
if ! grep -q "hello" "$TMPDIR/hello.tkir"; then
    echo "FAIL: string literal 'hello' not found in .tkir output"
    exit 1
fi
echo "  PASS: test_string_literal"

# --- Test 4: Function name in func section ---
cat > "$TMPDIR/addfn.tk" <<'EOF'
m=test;
f=add(a:i64;b:i64):i64{
  <a+b
};
f=main():i64{
  <add(3;4)
};
EOF

"$TKC" --emit-tkir "$TMPDIR/addfn.tk" --out "$TMPDIR/addfn.tkir"

if ! grep -q "add" "$TMPDIR/addfn.tkir"; then
    echo "FAIL: function name 'add' not found in .tkir output"
    exit 1
fi
if ! grep -q "main" "$TMPDIR/addfn.tkir"; then
    echo "FAIL: function name 'main' not found in .tkir output"
    exit 1
fi
echo "  PASS: test_function_names"

echo ""
echo "ALL tkir encoder tests PASSED"
