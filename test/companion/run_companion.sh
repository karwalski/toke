#!/usr/bin/env bash
# Test runner for --verify-companion flag.
# Story: 11.5.6
set -uo pipefail
PASS=0 FAIL=0
TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="/tmp/tkc_companion_test"
rm -rf "$TMP" && mkdir -p "$TMP"

# ── Test 1: MATCH — generate companion, then verify it ──────────────
echo "==> Test 1: MATCH (generate then verify)"
SRC="${DIR}/../../bench/programs/prime_sieve.tk"
COMP="${TMP}/prime_sieve.tkc.md"
cp "$SRC" "${TMP}/prime_sieve.tk"
"$TKC" --companion-out "$COMP" "${TMP}/prime_sieve.tk" 2>/dev/null
OUT=$("$TKC" --verify-companion "$COMP" 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$OUT" | grep -q "^MATCH:"; then
    echo "  PASS (exit=$rc, output: $OUT)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 2: MISMATCH — modify source after generating companion ─────
echo "==> Test 2: MISMATCH (modify source after generate)"
echo " " >> "${TMP}/prime_sieve.tk"
OUT=$("$TKC" --verify-companion "$COMP" 2>&1); rc=$?
if [ $rc -eq 1 ] && echo "$OUT" | grep -q "^MISMATCH:"; then
    echo "  PASS (exit=$rc, output: $OUT)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 3: ERROR — companion file not found ─────────────────────────
echo "==> Test 3: ERROR (companion not found)"
OUT=$("$TKC" --verify-companion "${TMP}/nonexistent.tkc.md" 2>&1); rc=$?
if [ $rc -eq 2 ]; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 4: ERROR — source file not found ────────────────────────────
echo "==> Test 4: ERROR (source file not found)"
cat > "${TMP}/bad_source.tkc.md" <<'YAML'
---
source_file: "does_not_exist.tk"
source_hash: "0000000000000000000000000000000000000000000000000000000000000000"
compiler_version: "0.1.0"
generated_at: "2026-04-04T00:00:00Z"
format_version: "1.0"
---
YAML
OUT=$("$TKC" --verify-companion "${TMP}/bad_source.tkc.md" 2>&1); rc=$?
if [ $rc -eq 2 ]; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 5: ERROR — missing source_hash field ────────────────────────
echo "==> Test 5: ERROR (missing source_hash)"
cat > "${TMP}/no_hash.tkc.md" <<'YAML'
---
source_file: "prime_sieve.tk"
compiler_version: "0.1.0"
format_version: "1.0"
---
YAML
OUT=$("$TKC" --verify-companion "${TMP}/no_hash.tkc.md" 2>&1); rc=$?
if [ $rc -eq 2 ]; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 6: ERROR — no argument provided ─────────────────────────────
echo "==> Test 6: ERROR (no argument)"
OUT=$("$TKC" --verify-companion 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Summary ──────────────────────────────────────────────────────────
rm -rf "$TMP"
echo "---"
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
