#!/usr/bin/env bash
# C078_e9003_source_line.sh — a position-less diagnostic carries an empty
# source_line, never stray bytes.  (Diagnostic-series in content; numbered in
# the C range because the YAML runner is --check only and cannot reach the
# link step where E9003 is emitted.)
#
# Story: 127.181.
#
# THE DEFECT.  E9003 "clang invocation failed" is emitted after code
# generation, with no toke position (offset 0, line 0).  diag.c still copied
# "the line at offset 0" out of whatever source buffer was registered:
#   * multi-module build (tkc a.tk b.tk -o bin): the driver had already
#     freed that buffer, so source_line carried heap garbage ("\u0006",
#     "\u0007", ...) -- a read of freed memory inside the machine-readable
#     record the repair loop consumes;
#   * single-module build: it carried line 1 of the program, pointing the
#     reader at a line that has nothing to do with the failure.
#
# HOW IT FORCES E9003.  -o into a directory that does not exist: the IR is
# fine, clang's link step cannot write the output.  The multi-module case is
# repeated because the garbage depends on the allocator.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_e9003sl_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C078: E9003 has an empty source_line"
echo "------------------------------------"

cat > lib.tk <<'EOF'
m=pair.lib;
f=twice(x:i64):i64{<x*2};
EOF
cat > app.tk <<'EOF'
m=pair.app;
i=lib:pair.lib;
f=main():i64{<lib.twice(0)};
EOF
cat > single.tk <<'EOF'
m=single;
f=main():i64{<0};
EOF

NOWHERE="${WORK}/no/such/dir/bin"

expect_empty() {
    local name="$1"; shift
    "${TKC}" "$@" -o "${NOWHERE}" > out.log 2>&1
    local rec
    rec="$(grep '"error_code":"E9003"' out.log | head -1)"
    if [ -z "${rec}" ]; then
        echo "  FAIL: ${name}: no E9003 record"
        head -3 out.log | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return
    fi
    case "${rec}" in
        *'"source_line":""}') echo "  PASS: ${name}"; PASS=$((PASS + 1)) ;;
        *) echo "  FAIL: ${name}: ${rec##*,}"; FAIL=$((FAIL + 1)) ;;
    esac
}

expect_empty "single module" single.tk
for n in 1 2 3 4 5; do
    expect_empty "multi module, run ${n}" lib.tk app.tk
done

echo ""
echo "C078: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
