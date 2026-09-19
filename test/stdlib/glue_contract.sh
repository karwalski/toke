#!/bin/bash
# glue_contract.sh -- Epic 136.16-136.25: the _w glue honours the contract that
# the .tki interface, the canonical documentation and the C core already agree
# on.
#
# Every defect in that group had the same shape: the glue wrapper dropped or
# reinterpreted an argument, so the module compiled, linked and produced a
# plausible-looking answer that was not the documented one. `make check-docs`
# proves such an example COMPILES. Nothing there proves it BEHAVES. This script
# is the behavioural half: it compiles real .tk consumers with tkc and runs
# them, comparing stdout against the values the documentation promises.
#
# Where the defect was about process-global state (136.18, 136.19) or about
# persistence, the check runs the program MORE THAN ONCE, or exercises two
# distinct values in one process, because a single-value probe cannot tell a
# per-argument implementation apart from a hidden global that happens to hold
# the right thing.
#
# Exit 0 = pass, 1 = fail.

set -u

TKC="${TKC:-./toke}"
STDLIB="${TKC_STDLIB_DIR:-$PWD/src/stdlib}"
TMP="$(mktemp -d)"
export TKC_STDLIB_DIR="$STDLIB"

pass=0
fail=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# build <name> <src> -> $TMP/<name>; fails loudly, never silently skips.
build() {
    local name="$1" src="$2"
    if ! "$TKC" --out "$TMP/$name" "$src" >"$TMP/$name.build.log" 2>&1; then
        echo "FAIL [$name] compile:"
        sed 's/^/        /' "$TMP/$name.build.log"
        fail=$((fail + 1))
        return 1
    fi
    return 0
}

# expect <label> <actual> <expected>
expect() {
    local label="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo "PASS $label"
        pass=$((pass + 1))
    else
        echo "FAIL $label"
        echo "        expected: $expected"
        echo "        got:      $actual"
        fail=$((fail + 1))
    fi
}

# line <output> <key> -> the value after "<key>=" on its own line
line() { printf '%s\n' "$1" | grep "^$2=" | head -1 | cut -d= -f2-; }

echo "=== 136.20 / 127.84 -- math.min and math.max over an array ==="
if build math_minmax test/stdlib/math_minmax.tk; then
    out="$("$TMP/math_minmax")"
    # The negative cases are the ones the old sign-bit compare inverted.
    expect "math.min(@(-1.0;-5.0;-3.0)) == -5"  "$(line "$out" neg.min)"   "-5"
    expect "math.max(@(-1.0;-5.0;-3.0)) == -1"  "$(line "$out" neg.max)"   "-1"
    expect "math.min(@(3.0;-2.5;7.0;0.0)) == -2.5" "$(line "$out" mixed.min)" "-2.5"
    expect "math.max(@(3.0;-2.5;7.0;0.0)) == 7"    "$(line "$out" mixed.max)" "7"
    expect "math.min over positives == 1"       "$(line "$out" pos.min)"   "1"
    expect "math.max over positives == 5"       "$(line "$out" pos.max)"   "5"
    expect "math.min of a single element"       "$(line "$out" one.min)"   "-42"
    expect "math.max of a single element"       "$(line "$out" one.max)"   "-42"
fi

echo
echo "=== 136.16 -- std.toon typed accessors take (Toon; key) ==="
if build toon_accessors test/stdlib/toon_accessors.tk; then
    out="$("$TMP/toon_accessors")"
    expect "toon.str(t; \"name\") == Alice"  "$(line "$out" str.name)"       "Alice"
    expect "toon.i64(t; \"id\") == 1"        "$(line "$out" i64.id)"         "1"
    expect "toon.f64(t; \"score\") == 1.5"   "$(line "$out" f64.score)"      "1.5"
    expect "toon.bool(t; \"active\") is ok"  "$(line "$out" bool.active)"    "1"
    # A key that is absent must take the \$err arm, not return the document.
    expect "toon.str of an absent key errs"  "$(line "$out" str.missing)"    "ERR"
    # arr returned a freshly calloc'd EMPTY array for every input before this.
    expect "toon.arr(t; \"name\") has 3 rows" "$(line "$out" arr.name.len)"  "3"
    expect "toon.arr(t; \"id\") has 3 rows"   "$(line "$out" arr.id.len)"    "3"
    expect "toon.arr of an absent key is empty" "$(line "$out" arr.missing.len)" "0"
fi

echo
echo "=== 136.23 -- file.append(path; content), across two processes ==="
if build file_append test/stdlib/file_append.tk; then
    APPEND_FILE="$TMP/append.txt"
    rm -f "$APPEND_FILE"
    out1="$(APPENDPATH="$APPEND_FILE" "$TMP/file_append" --allow-write --allow-read)"
    # A SECOND process, so a pass means the bytes reached the filesystem
    # rather than an in-process buffer, and that append did not truncate.
    out2="$(APPENDPATH="$APPEND_FILE" "$TMP/file_append" --allow-write --allow-read)"
    expect "file.append reports success"        "$(line "$out1" append.rc)" "1"
    expect "first process leaves 5 bytes"       "$(line "$out1" bytes)"     "5"
    expect "second process appends, not truncates" "$(line "$out2" bytes)"  "10"
    expect "the file holds both lines"          "$(wc -l < "$APPEND_FILE" | tr -d ' ')" "2"
fi

echo
echo "glue_contract: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
