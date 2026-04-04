#!/usr/bin/env bash
# Migration test runner for tkc --migrate.
#
# For each *.legacy.tk file, runs tkc --migrate and:
#   1. Compares output against the .expected file (text diff).
#   2. Verifies the migrated output compiles in default mode.
#
# Story: 11.3.5
set -uo pipefail
PASS=0 FAIL=0
TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
DIR="$(cd "$(dirname "$0")" && pwd)"

for legacy in "${DIR}"/*.legacy.tk; do
    name=$(basename "$legacy" .legacy.tk)
    exp_file="${DIR}/${name}.expected"

    if [ ! -f "$exp_file" ]; then
        echo "SKIP $name (no .expected file)"
        continue
    fi

    # Run migration
    actual=$("$TKC" --migrate "$legacy" 2>/tmp/tkc_migrate_err)
    rc=$?
    if [ "$rc" -ne 0 ]; then
        echo "FAIL $name (tkc --migrate exited $rc)"
        cat /tmp/tkc_migrate_err
        FAIL=$((FAIL+1))
        continue
    fi

    # Compare output to expected
    expected=$(cat "$exp_file")
    if [ "$actual" != "$expected" ]; then
        echo "FAIL $name (output mismatch)"
        diff <(echo "$expected") <(echo "$actual") || true
        FAIL=$((FAIL+1))
        continue
    fi

    # Verify migrated output compiles in default mode
    tmp="/tmp/tkc_migrate_${name}.tk"
    echo "$actual" > "$tmp"
    if ! "$TKC" --check "$tmp" 2>/tmp/tkc_migrate_check_err; then
        echo "FAIL $name (migrated output does not compile)"
        cat /tmp/tkc_migrate_check_err
        rm -f "$tmp"
        FAIL=$((FAIL+1))
        continue
    fi
    rm -f "$tmp"

    echo "PASS $name"
    PASS=$((PASS+1))
done

echo "---"
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
