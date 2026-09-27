#!/usr/bin/env bash
# C077_multi_module_link.sh — `tkc a.tk b.tk -o bin` links several modules into
# ONE binary, and that binary runs.
#
# Story: 127.76.
#
# THE DEFECT.  114.40's multi-source build compiles each module to its own
# .ll and links them in one clang call.  From a3adceb (2026-07-07) until
# 127.76 the driver space-joined the .ll paths into one string and
# compile_binary passed that string to clang as ONE argv element, so every
# multi-source build — v3.0.0 included — failed with
#   clang: error: no such file or directory: '/tmp/tkc_proj_X/m0.ll /tmp/.../m1.ll'
# followed by E9003.  Nothing built a multi-module program, so no gate saw it.
#
# WHY VALUES.  A successful link proves the argv only; running the binary and
# comparing its output proves the cross-module call reached the other module.
# A third case lists THREE modules, so a fix that special-cased two would fail.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_multilink_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C077: a multi-module program links into one binary and runs"
echo "------------------------------------------------------------"

cat > lib.tk <<'EOF'
m=pair.lib;
f=twice(x:i64):i64{<x*2};
EOF
cat > mid.tk <<'EOF'
m=pair.mid;
i=lib:pair.lib;
f=quad(x:i64):i64{<lib.twice(lib.twice(x))};
EOF
cat > app2.tk <<'EOF'
m=pair.app;
i=io:std.io;
i=lib:pair.lib;
f=main():i64{io.println("twice 21 = \(lib.twice(21))");<0};
EOF
cat > app3.tk <<'EOF'
m=pair.app;
i=io:std.io;
i=lib:pair.lib;
i=mid:pair.mid;
f=main():i64{io.println("\(lib.twice(5)) \(mid.quad(5))");<0};
EOF

links() {
    local name="$1" expected="$2"; shift 2
    rm -f bin
    if ! "${TKC}" "$@" -o bin > build.log 2>&1 || [ ! -x bin ]; then
        echo "  FAIL: ${name}: build failed"
        grep -v '^{' build.log | head -2 | sed 's/^/      /'
        grep -o '"error_code":"[^"]*","severity":"error"[^}]*"message":"[^"]*"' build.log \
            | head -1 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return
    fi
    local out
    out="$(./bin 2>&1)"
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${expected}', got '${out}'"
        FAIL=$((FAIL + 1))
    fi
}

links "two modules"   "twice 21 = 42" lib.tk app2.tk
links "three modules" "10 20"         lib.tk mid.tk app3.tk

echo ""
echo "C077: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
