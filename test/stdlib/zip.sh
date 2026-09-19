#!/usr/bin/env bash
# zip.sh — std.zip behavioural conformance (story 135.1).
#
# This is the behavioural half of the gate, not the compile half. `make
# check-docs` proves a documented example COMPILES; it cannot tell a reader
# that decompresses correctly from one that returns the right-shaped garbage,
# and it cannot tell a security check that fires from one that was never
# reached. So every case here RUNS a compiled consumer and compares its output
# against numbers computed independently by test/stdlib/zip_fixtures.py.
#
# The cases, in the order they run:
#
#   1. LINK — a program importing std.zip AND NOTHING ELSE must build. 136.33
#      registered six modules' glue against the wrong module name; that defect
#      is invisible unless the module is the only import, because any other
#      import drags the glue in.
#   2. HAPPY — entry names, both sizes, the directory flag, and the exact
#      decompressed bytes of a text entry and a non-UTF-8 binary entry. Every
#      declared $zipentry field is read, because a .tki/glue slot mismatch
#      still compiles and then returns whatever sits at the offset (127.86).
#   3. REJECTIONS — seven archives, each refused BY NAME. "It returned an
#      error" is not asserted anywhere; the rule that fired is.
#
# Exit 0 = pass, 1 = fail.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# The repo's tkc is a symlink that any concurrent `make` relinks (131.39), so
# a caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

pass=0
fail=0

expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"
        pass=$((pass + 1))
    else
        echo "FAIL $label"
        echo "        expected: $want"
        echo "        got:      $actual"
        fail=$((fail + 1))
    fi
}

# The value after "<key>=" on its own line of the program output.  Literal
# prefix match, not a regex: entry keys contain '/' and '.' (read-nested/deep.bin).
line() {
    printf '%s\n' "$1" |
        awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); exit }'
}

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_zip_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "std.zip — read-only archive access (135.1)"
echo "------------------------------------------"

# ── 1. std.zip alone must link ───────────────────────────────────────────
cat > "${WORK}/zonly.tk" <<'EOF'
m=zonly;
i=zip:std.zip;
f=main():i64{
  let a=zip.openfile("/nonexistent/none.zip");
  mt a { $ok:z 1; $err:e 0 }
};
EOF
if "${TKC}" --out "${WORK}/zonly" "${WORK}/zonly.tk" > "${WORK}/zonly.log" 2>&1; then
    expect "link-std-zip-alone" "ok" "ok"
else
    expect "link-std-zip-alone" "$(tail -3 "${WORK}/zonly.log")" "ok"
fi

# ── Fixtures ─────────────────────────────────────────────────────────────
FIX="${WORK}/fixtures"
if ! FACTS="$(python3 "${SCRIPT_DIR}/zip_fixtures.py" "${FIX}" 2>"${WORK}/fix.log")"; then
    echo "FAIL: fixture generation"
    sed 's/^/        /' "${WORK}/fix.log"
    exit 1
fi
hello_size="$(line "$FACTS" hello_size)"
hello_csize="$(line "$FACTS" hello_csize)"
hello_bytes="$(line "$FACTS" hello_bytes)"
deep_size="$(line "$FACTS" deep_size)"
deep_csize="$(line "$FACTS" deep_csize)"
deep_bytes="$(line "$FACTS" deep_bytes)"
empty_csize="$(line "$FACTS" empty_csize)"
dir_csize="$(line "$FACTS" dir_csize)"

# ── Build and run the consumer ───────────────────────────────────────────
if ! "${TKC}" --out "${WORK}/consumer" "${SCRIPT_DIR}/zip_consumer.tk" \
        > "${WORK}/build.log" 2>&1; then
    echo "FAIL: zip_consumer.tk did not build"
    sed 's/^/        /' "${WORK}/build.log"
    exit 1
fi

OUT="$(ZIPFIX="${FIX}" "${WORK}/consumer" --allow-read 2>&1)"
rc=$?
if [ $rc -ne 0 ]; then
    echo "FAIL: consumer exited ${rc}"
    printf '%s\n' "$OUT" | sed 's/^/        /'
    exit 1
fi

# ── 2. The honest archive ────────────────────────────────────────────────
expect "open-good"      "$(line "$OUT" open-good)" "ok"
expect "entry-count"    "$(line "$OUT" count)"     "4"

# Order is the archive's own central-directory order (the order written).
expect "entry-0-hello"  "$(line "$OUT" e0)" "hello.txt|${hello_size}|${hello_csize}|0"
expect "entry-1-empty"  "$(line "$OUT" e1)" "empty.txt|0|${empty_csize}|0"
expect "entry-2-dir"    "$(line "$OUT" e2)" "nested/|0|${dir_csize}|1"
expect "entry-3-binary" "$(line "$OUT" e3)" "nested/deep.bin|${deep_size}|${deep_csize}|0"

# Exact bytes, not "some bytes came back".
expect "read-text"      "$(line "$OUT" read-hello.txt)" "${hello_size}|${hello_bytes}"
# A binary entry containing 0x00 and 0xFF: proves @(byte) is not a C string.
expect "read-binary"    "$(line "$OUT" read-nested/deep.bin)" "${deep_size}|${deep_bytes}"
# An empty entry is empty bytes, NOT an error — the two must stay distinct.
expect "read-empty"     "$(line "$OUT" read-empty.txt)" "0|"
# And the two ways of having no content to return must both be errors.
expect "read-absent"    "$(line "$OUT" read-absent.txt)" "ERR|zip: no such entry"
expect "read-dir"       "$(line "$OUT" read-nested/)"    "ERR|zip: entry is a directory"

# ── 3. Each security rule, rejected by name ──────────────────────────────
expect "reject-traversal" "$(line "$OUT" reject-traversal)" \
       "zip: path traversal in entry name"
expect "reject-absolute"  "$(line "$OUT" reject-absolute)" \
       "zip: absolute path in entry name"
expect "reject-backslash" "$(line "$OUT" reject-backslash)" \
       "zip: backslash in entry name"
expect "reject-sizecap"   "$(line "$OUT" reject-size)" \
       "zip: uncompressed size cap exceeded"
expect "reject-ratiocap"  "$(line "$OUT" reject-ratio)" \
       "zip: compression ratio cap exceeded"
expect "reject-countcap"  "$(line "$OUT" reject-count)" \
       "zip: entry count cap exceeded"
expect "reject-notazip"   "$(line "$OUT" reject-notzip)" \
       "zip: not a zip archive, or its central directory is corrupt"

echo "------------------------------------------"
echo "Results: ${pass} passed, ${fail} failed"
[ "${fail}" -eq 0 ] || exit 1
exit 0
