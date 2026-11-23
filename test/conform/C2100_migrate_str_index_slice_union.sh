#!/usr/bin/env bash
# C2100_migrate_str_index_slice_union.sh — `tkc --migrate` rewrites a legacy
# str index used as a str to `.slice(i;i+1)`, which is `str!$sliceerr` since
# 127.462, and says so; it writes no filler error arm (story 127.510).
#
# THE FINDING. `make test-ci-green` was red on test-migrate str_index: the
# migrated text matched its .expected byte for byte, but the output stopped
# compiling when 127.462 made the receiver slice a union (five E4031s). The
# migrator was not changed by 127.462; its rewrite target was.
#
# THE DECISION (conservative, 127.510). --migrate is a mechanical rewriter and
# owner answer R2 (2026-11-15) rules out a mechanical filler rewrite: it may
# not write `mt ..{$ok:v v;$err:e ""}`. So the `.slice(i;i+1)` stays a union,
# the author gives it an error arm, and --migrate now prints a note naming the
# rewrite's type. A numeric-context index still becomes `.charat(i)` (no union).
#
# WHAT IS ASSERTED
#   1. A str-context index migrates to `.slice(i;i+1)`, a numeric one to
#      `.charat(i)`, exit 0.
#   2. stderr carries the str!$sliceerr note; with no str-context index it
#      does not.
#   3. No filler arm is written (`$err:` does not appear in the output).
#   4. The migrated output's only diagnostics are E4031 'str!$sliceerr'.
#   5. The charat-only output compiles.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2100_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C2100: --migrate str index -> .slice is a union, with a note and no filler"
echo "--------------------------------------------------------------------------"

cat > "${WORK}/s.legacy.tk" <<'EOF'
M=x;
I=s:std.str;
F=first(v:Str):Str{<v[0]};
F=digit(v:Str):i64{let c=v[0];<c-48};
F=main():i64{s.print(first("ab"));<digit("7")};
EOF
cat > "${WORK}/b.legacy.tk" <<'EOF'
M=x;
I=s:std.str;
F=digit(v:Str):i64{let c=v[0];<c-48};
F=main():i64{<digit("7")};
EOF

"${TKC}" --migrate "${WORK}/s.legacy.tk" >"${WORK}/s.tk" 2>"${WORK}/s.err"; rc=$?
if [ "${rc}" -eq 0 ] && grep -q 'f=first(v:str):str{<v.slice(0;1)};' "${WORK}/s.tk" \
   && grep -q 'let c=v.charat(0);' "${WORK}/s.tk"; then
    pass "str context -> .slice(0;1), numeric context -> .charat(0), rc 0"
else
    fail "rc=${rc}, output: $(cat "${WORK}/s.tk")"
fi

if grep -q "note: each .slice(i;i+1) is str!\$sliceerr" "${WORK}/s.err"; then
    pass "stderr notes the slice union"
else
    fail "no slice-union note: $(cat "${WORK}/s.err")"
fi

if grep -q '\$err' "${WORK}/s.tk"; then
    fail "a filler error arm was written: $(cat "${WORK}/s.tk")"
else
    pass "no filler error arm"
fi

diags="$("${TKC}" --check "${WORK}/s.tk" 2>&1 | grep -o '"error_code":"[A-Z0-9]*"\|"got":"[^"]*"' | tr '\n' ' ')"
if [ "${diags}" = '"error_code":"E4031" "got":"str!$sliceerr" ' ]; then
    pass "migrated output: exactly one E4031 str!\$sliceerr"
else
    fail "migrated output diagnostics: ${diags}"
fi

"${TKC}" --migrate "${WORK}/b.legacy.tk" >"${WORK}/b.tk" 2>"${WORK}/b.err"; rc=$?
if [ "${rc}" -eq 0 ] && ! grep -q 'sliceerr' "${WORK}/b.err"; then
    pass "charat-only migration: no slice note"
else
    fail "rc=${rc}, stderr: $(cat "${WORK}/b.err")"
fi
if "${TKC}" --check "${WORK}/b.tk" >"${WORK}/b.chk" 2>&1; then
    pass "charat-only output compiles"
else
    fail "charat-only output: $(head -c 300 "${WORK}/b.chk")"
fi

echo ""
echo "C2100: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
