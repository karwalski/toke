#!/usr/bin/env bash
# Migration test runner for tkc --migrate.
#
# For each *.legacy.tk file, runs tkc --migrate and:
#   1. Compares output against the .expected file (text diff).
#   2. Verifies the migrated output compiles in default mode.
#
# 127.57: two of the fixtures here — migrate_add and migrate_struct — already
# compile as they stand, and their .expected files are byte-identical copies of
# their inputs. They are no-op cases, and since 2026-09-24 --migrate DECLINES
# on source that compiles: it echoes the input verbatim (so the .expected
# comparison still holds, exactly) and reports the refusal with a non-zero exit
# code. The expected exit code is therefore derived from the fixture rather
# than assumed to be 0 — `--check` decides, the same way the compiler does.
#
# Story: 11.3.5, 127.57, 127.197, 127.510
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

    # 127.57: a fixture that already compiles is a no-op case; --migrate must
    # decline it (non-zero) rather than rewrite source that needs no rewriting.
    # 127.197: a decline exits 4 (TKC_EXIT_DECLINED, E9026), not 1.
    if "$TKC" --check "$legacy" >/dev/null 2>&1; then want_rc=4; else want_rc=0; fi

    # Run migration
    actual=$("$TKC" --migrate "$legacy" 2>/tmp/tkc_migrate_err)
    rc=$?
    if [ "$rc" -ne "$want_rc" ]; then
        echo "FAIL $name (tkc --migrate exited $rc, expected $want_rc)"
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
    # 127.510: a fixture with a <name>.check file migrates to source that is
    # EXPECTED not to compile: the file lists, one per line, `line:col CODE
    # got` for every diagnostic --check must report, and nothing else.  This
    # is for a rewrite whose target is an error union (a str index becomes
    # `.slice(i;i+1)`, `str!$sliceerr` since 127.462): --migrate leaves the
    # error arm to the author (owner answer R2: no mechanical filler).
    chk_file="${DIR}/${name}.check"
    if [ -f "$chk_file" ]; then
        got_diags=$("$TKC" --check "$tmp" 2>&1 | python3 -c '
import json, sys
for l in sys.stdin:
    l = l.strip()
    if not l.startswith("{"): continue
    d = json.loads(l)
    print("%d:%d %s %s" % (d["pos"]["line"], d["pos"]["col"], d["error_code"], d.get("got", "")))')
        if [ "$got_diags" != "$(cat "$chk_file")" ]; then
            echo "FAIL $name (migrated output's --check diagnostics differ from ${name}.check)"
            diff <(cat "$chk_file") <(echo "$got_diags") || true
            rm -f "$tmp"
            FAIL=$((FAIL+1))
            continue
        fi
        rm -f "$tmp"
        echo "PASS $name (expected diagnostics)"
        PASS=$((PASS+1))
        continue
    fi
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
