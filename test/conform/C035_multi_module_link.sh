#!/usr/bin/env bash
# C035_multi_module_link.sh — `tkc a.tk b.tk main.tk --out bin` links several
# modules into ONE binary that runs (114.40).
#
# 121.1b moved the clang call to argv-exec, so nothing splits a string on
# whitespace any more.  The multi-module path still joined every per-module
# .ll path into one space-separated string and passed it as a single argv
# element, so clang looked for a file literally named
# "/tmp/tkc_proj_X/m0.ll /tmp/tkc_proj_X/m1.ll" and every multi-file build
# failed with E9003.  examples/mortgage was unbuildable.
#
# Cases: two and three modules (each .ll must reach clang), and an output
# path containing a space (the property 121.1b exists to protect).

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

WORK="$(mktemp -d /tmp/tkc_multimod_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C035: several modules link into one runnable binary"
echo "--------------------------------------"

cat > model.tk <<'EOF'
m=proj.model;
t=$pt{x:i64;y:i64};
EOF

cat > calc.tk <<'EOF'
m=proj.calc;
i=md:proj.model;
f=dist2(p:$pt):i64{<p.x*p.x+p.y*p.y};
f=triple(n:i64):i64{<n*3};
EOF

cat > main3.tk <<'EOF'
m=proj.main;
i=io:std.io;
i=md:proj.model;
i=calc:proj.calc;
f=main():i64{
  let p=$pt{x:3;y:4};
  io.println("dist2=\(calc.dist2(p)) triple=\(calc.triple(7))");
  <0
};
EOF

cat > lib2.tk <<'EOF'
m=two.lib;
f=sq(n:i64):i64{<n*n};
EOF

cat > main2.tk <<'EOF'
m=two.main;
i=io:std.io;
i=lib:two.lib;
f=main():i64{
  io.println("sq=\(lib.sq(12))");
  <0
};
EOF

# run_case NAME EXPECTED_STDOUT OUT_BIN FILE...
run_case() {
    local name="$1" expected="$2" bin="$3"; shift 3
    rm -f "${bin}"
    if ! "${TKC}" "$@" --out "${bin}" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log
        FAIL=$((FAIL + 1)); return
    fi
    local out rc=0
    out="$("./${bin}" 2>&1)" || rc=$?
    if [ "${rc}" -eq 0 ] && [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: exit ${rc}, expected '${expected}', got '${out}'"
        FAIL=$((FAIL + 1))
    fi
}

run_case "two modules"            "sq=144"                   "bin2"       lib2.tk main2.tk
run_case "three modules"          "dist2=25 triple=21"       "bin3"       model.tk calc.tk main3.tk
run_case "output path with space" "dist2=25 triple=21"       "out bin"    model.tk calc.tk main3.tk

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
