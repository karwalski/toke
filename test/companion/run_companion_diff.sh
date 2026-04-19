#!/usr/bin/env bash
# Test runner for --companion-diff flag.
# Story: 11.5.4
set -uo pipefail
PASS=0 FAIL=0
TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMP="/tmp/tkc_companion_diff_test"
rm -rf "$TMP" && mkdir -p "$TMP"

# ── Test 1: OK — no divergences (generate then diff) ─────────────────
echo "==> Test 1: OK (no divergences)"
SRC="${DIR}/../../bench/programs/prime_sieve.tk"
cp "$SRC" "${TMP}/prime_sieve.tk"
COMP="${TMP}/prime_sieve.tkc.md"
"$TKC" --companion-out "$COMP" "${TMP}/prime_sieve.tk" 2>/dev/null
OUT=$("$TKC" --companion-diff "$COMP" "${TMP}/prime_sieve.tk" 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$OUT" | grep -q "^OK:"; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 2: HASH mismatch — modify source after generating companion ──
echo "==> Test 2: HASH mismatch (source changed)"
echo " " >> "${TMP}/prime_sieve.tk"
OUT=$("$TKC" --companion-diff "$COMP" "${TMP}/prime_sieve.tk" 2>&1); rc=$?
if [ $rc -eq 1 ] && echo "$OUT" | grep -q "^HASH"; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 3: NEW function — add a function to source ──────────────────
echo "==> Test 3: NEW function (source has extra function)"
# Restore original and generate companion
cp "$SRC" "${TMP}/new_func.tk"
"$TKC" --companion-out "${TMP}/new_func.tkc.md" "${TMP}/new_func.tk" 2>/dev/null
# Now add a function to the source
cat >> "${TMP}/new_func.tk" <<'TK'
f=added():i64{<42};
TK
OUT=$("$TKC" --companion-diff "${TMP}/new_func.tkc.md" "${TMP}/new_func.tk" 2>&1); rc=$?
if [ $rc -eq 1 ] && echo "$OUT" | grep -q "^NEW.*function.*added"; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 4: REMOVED function — companion has function not in source ──
echo "==> Test 4: REMOVED function (companion has extra function)"
# Generate companion with the extra function, then diff against original
"$TKC" --companion-out "${TMP}/removed.tkc.md" "${TMP}/new_func.tk" 2>/dev/null
cp "$SRC" "${TMP}/removed.tk"
OUT=$("$TKC" --companion-diff "${TMP}/removed.tkc.md" "${TMP}/removed.tk" 2>&1); rc=$?
if [ $rc -eq 1 ] && echo "$OUT" | grep -q "^REMOVED.*function.*added"; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 5: CHANGED signature — modify function params ───────────────
echo "==> Test 5: CHANGED signature (parameter change)"
# Create source with isprime(n:i64;x:i64) — different from companion's isprime(n:i64)
cat > "${TMP}/changed.tk" <<'TK'
m=bench;
f=isprime(n:i64;x:i64):i64{<0};
f=main():i64{<0};
TK
# Generate companion from original (single param)
cat > "${TMP}/changed_orig.tk" <<'TK'
m=bench;
f=isprime(n:i64):i64{<0};
f=main():i64{<0};
TK
"$TKC" --companion-out "${TMP}/changed.tkc.md" "${TMP}/changed_orig.tk" 2>/dev/null
OUT=$("$TKC" --companion-diff "${TMP}/changed.tkc.md" "${TMP}/changed.tk" 2>&1); rc=$?
if [ $rc -eq 1 ] && echo "$OUT" | grep -q "^CHANGED.*function.*isprime"; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 6: ERROR — companion file not found ─────────────────────────
echo "==> Test 6: ERROR (companion not found)"
OUT=$("$TKC" --companion-diff "${TMP}/nonexistent.tkc.md" "${TMP}/prime_sieve.tk" 2>&1); rc=$?
if [ $rc -eq 2 ]; then
    echo "  PASS (exit=$rc)"
    PASS=$((PASS+1))
else
    echo "  FAIL (exit=$rc, output: $OUT)"
    FAIL=$((FAIL+1))
fi

# ── Test 7: ERROR — no argument ──────────────────────────────────────
echo "==> Test 7: ERROR (no argument)"
OUT=$("$TKC" --companion-diff 2>&1); rc=$?
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
