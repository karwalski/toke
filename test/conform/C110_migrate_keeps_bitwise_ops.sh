#!/usr/bin/env bash
# C110_migrate_keeps_bitwise_ops.sh — `--migrate` keeps ^ ~ << >> (story 127.213).
#
# THE DEFECT.  src/migrate.c dropped TK_CARET, TK_TILDE, TK_SHL and TK_SHR
# with a warning "bitwise '^' ... removed (v0.5)".  All four are live
# operators in both syntaxes (spec v0.4 §charset; 114.8 assigned ^ and ~), so
# the migrated text was different code:
#   let b=a^3   ->  let b=a3    (E3011, unknown name — loud)
#   let g=2^3   ->  let g=23    (compiles, prints 23 instead of 1 — silent)
#   let d=a<<2  ->  let d=a2    and  ~a -> a  (silent sign change)
#
# WHAT THIS DRIVES.  A legacy program using each operator is (1) built and
# run in --legacy mode, (2) migrated, (3) the migration output is compared
# with the expected default-syntax text byte for byte, and (4) the migrated
# program is built and run; its output must equal the legacy run's.  Both
# shapes named by the row are covered: an identifier operand (a^3) and a
# literal-literal operand pair (2^3), which is the silent one.
#
# Negative control: restore the old `case TK_CARET: case TK_TILDE:` /
# `case TK_SHL: case TK_SHR:` arms that print the warning and `break` —
# checks 2, 3 and 4 fail (text "a3", "23"; migrated build fails E3011).
#
# Story: 127.213

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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c110_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C110: --migrate keeps the live bitwise operators"
echo "------------------------------------------------"

# One value per line so a wrong operator cannot hide in a sum.
cat > "${WORK}/ops.legacy.tk" <<'EOF'
M=x;I=j:std.json;F=main():i64{let a=5;let b=a^3;let c=~a;let d=a<<2;let e=a>>1;let g=2^3;j.print(b);j.print(c);j.print(d);j.print(e);j.print(g);<0};
EOF
cat > "${WORK}/ops.expected" <<'EOF'
m=x;i=j:std.json;f=main():i64{let a=5;let b=a^3;let c=~a;let d=a<<2;let e=a>>1;let g=2^3;j.print(b);j.print(c);j.print(d);j.print(e);j.print(g);<0};
EOF
# 5^3=6, ~5=-6, 5<<2=20, 5>>1=2, 2^3=1 (one per line).
WANT="6 -6 20 2 1"

# 1. The legacy program itself means what we think it means.
if "${TKC}" --legacy "${WORK}/ops.legacy.tk" -o "${WORK}/legacy" >"${WORK}/l.err" 2>&1; then
    got="$("${WORK}/legacy" | tr "\n" " " | sed "s/ $//")"
    if [ "${got}" = "${WANT}" ]; then pass "legacy build prints ${WANT}"
    else fail "legacy build printed '${got}', want '${WANT}'"; fi
else
    fail "legacy build failed: $(head -c 300 "${WORK}/l.err")"
fi

# 2. Migration keeps every operator and warns about none of them.
"${TKC}" --migrate "${WORK}/ops.legacy.tk" >"${WORK}/ops.mig.tk" 2>"${WORK}/m.err"
rc=$?
if [ "${rc}" -eq 0 ] && cmp -s "${WORK}/ops.mig.tk" "${WORK}/ops.expected"; then
    pass "migrated text keeps ^ ~ << >>"
else
    fail "migrate rc=${rc}, output: $(cat "${WORK}/ops.mig.tk")"
fi
if /usr/bin/grep -a -q "removed (v0.5)" "${WORK}/m.err"; then
    fail "migrate still warns an operator was removed: $(cat "${WORK}/m.err")"
else
    pass "no 'removed (v0.5)' warning"
fi

# 3. The silent shape specifically: 2^3 must not become the literal 23.
if /usr/bin/grep -a -q "let g=23;" "${WORK}/ops.mig.tk"; then
    fail "2^3 migrated to the literal 23"
else
    pass "2^3 is not rewritten to 23"
fi

# 4. The migrated program builds and prints the same values.
if "${TKC}" "${WORK}/ops.mig.tk" -o "${WORK}/migrated" >"${WORK}/d.err" 2>&1; then
    got="$("${WORK}/migrated" | tr "\n" " " | sed "s/ $//")"
    if [ "${got}" = "${WANT}" ]; then pass "migrated build prints ${WANT}"
    else fail "migrated build printed '${got}', want '${WANT}'"; fi
else
    fail "migrated build failed: $(head -c 300 "${WORK}/d.err")"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
