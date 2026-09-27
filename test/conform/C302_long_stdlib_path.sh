#!/usr/bin/env bash
# C302_long_stdlib_path.sh — a std.http program builds, and --emit-deps
# lists every source whole, when the stdlib lives at a long path (story
# 138.14, loke N-1).
#
# THE DEFECT. The stdlib C source list was built in a fixed `char
# sources[8192]` (llvm.c compile_binary_multi, stdlib_deps.h ResolvedDeps,
# and an 8 KB copy in main.c --emit-deps).  std.http needs 70 sources, so
# from a stdlib directory of ~110+ characters the list was cut mid-path:
# the build failed E9003 "clang: no such file or directory: '/…/deepdeep'"
# and --emit-deps printed 45 of the 70 paths.  The list now has 64 KB, and a
# list that still does not fit is E9010 — never a cut path handed to clang.
#
# This suite copies the stdlib (src/stdlib and stdlib/, ~7 MB) under a
# directory whose path is ~200 characters: TKC_STDLIB_DIR is canonicalised
# with realpath(), so a symlink would not make the path long.
#
# Negative control: TKC built from main at c15dd11 — 1 red ("build-failed"),
# 2 red ("38 paths, 1 missing": the list cut mid-path at 8192 bytes).  The E9010
# overflow branch was shown by a build with -DTKC_DEPS_SOURCES_SIZE=4096
# (E9010 "compiler limit exceeded", no clang run).
#
# Story: 138.14

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# ~200 characters: past the ~117 at which 70 paths overflowed 8192 bytes,
# and under the 280-byte PATH_BUF probes elsewhere in the driver.
PAD=$(( (200 - ${#WORK} - 20) / 4 )); [ "${PAD}" -lt 1 ] && PAD=1
DEEP="${WORK}/$(printf 'deep%.0s' $(seq 1 ${PAD}))/checkout"
mkdir -p "${DEEP}/src"
cp -R "${REPO_ROOT}/src/stdlib" "${DEEP}/src/stdlib"
cp -R "${REPO_ROOT}/stdlib" "${DEEP}/stdlib"
SDIR="$(cd "${DEEP}/src/stdlib" && pwd -P)"
expect "0-path-is-long" "$([ "${#SDIR}" -ge 150 ] && echo long || echo "short ${#SDIR}")" "long"

cat > "${WORK}/h.tk" <<'EOF'
m=t;
i=h:std.http;
i=io:std.io;
f=main():i64{
  io.println("built");
  <0
};
EOF
rm -f "${WORK}/b"
TKC_STDLIB_DIR="${SDIR}" "${TKC}" "${WORK}/h.tk" --out "${WORK}/b" > "${WORK}/build.log" 2>&1
expect "1-http-builds-from-long-path" "$([ -x "${WORK}/b" ] && "${WORK}/b" || echo build-failed)" "built"

# every listed source is a whole path that exists
TKC_STDLIB_DIR="${SDIR}" "${TKC}" --emit-deps "${WORK}/h.tk" 2>/dev/null |
    awk '/^---/{exit} {print}' > "${WORK}/deps.txt"
n="$(wc -l < "${WORK}/deps.txt" | tr -d ' ')"
missing="$(while read -r f; do [ -f "$f" ] || echo x; done < "${WORK}/deps.txt" | wc -l | tr -d ' ')"
expect "2-emit-deps-whole" "$([ "${n}" -ge 60 ] && [ "${missing}" = 0 ] && echo ok || echo "${n} paths, ${missing} missing")" "ok"

echo "C302: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
