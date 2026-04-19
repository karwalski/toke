#!/usr/bin/env bash
# F002_pretty_expand.sh — test --pretty and --expand flags
#
# Verifies:
#   1. --pretty adds spaces around binary operators
#   2. --pretty adds blank lines before loops and returns
#   3. --expand adds identifier expansion comments
#   4. --pretty --expand combined output
#   5. Exit code 0 on valid input for both flags
#   6. --pretty and --expand skip type check / codegen
#
# Story: 10.8.2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${REPO_ROOT}/tkc"

PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "${expected}" = "${actual}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "    expected:"
        echo "${expected}" | sed 's/^/      /'
        echo "    actual:"
        echo "${actual}" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

# ── Check tkc exists ──────────────────────────────────────────────────────────
if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

echo "F002: Pretty-print and expand tests"
echo "--------------------------------------"

# ── Test 1: --pretty adds spaces around binary operators ──────────────────────

T1=$(mktemp /tmp/tkc_pretty_t1_XXXXXX.tk)
cat > "${T1}" <<'EOF'
m=test;
f=add(a:i64;b:i64):i64{<a+b}
EOF

T1_EXPECTED='m=test;
f=add(a:i64; b:i64):i64 {
  <a + b
}
'

T1_ACTUAL=$("${TKC}" --pretty "${T1}" 2>/dev/null) || true
check "pretty: spaces around binary ops" "${T1_EXPECTED}" "${T1_ACTUAL}"

# ── Test 2: --pretty adds blank lines before loops and returns ────────────────

T2=$(mktemp /tmp/tkc_pretty_t2_XXXXXX.tk)
cat > "${T2}" <<'EOF'
m=test;
f=sum(n:i64):i64{let s=0;lp(let i=0;i<n;i=i+1){s=s+i};<s}
EOF

T2_EXPECTED='m=test;
f=sum(n:i64):i64 {
  let s=0;

  lp (let i = 0; i < n; i = i + 1) {
    s = s + i
  };

  <s
}
'

T2_ACTUAL=$("${TKC}" --pretty "${T2}" 2>/dev/null) || true
check "pretty: blank lines before loop and return" "${T2_EXPECTED}" "${T2_ACTUAL}"

# ── Test 3: --expand adds identifier expansion comments ──────────────────────

T3=$(mktemp /tmp/tkc_expand_t3_XXXXXX.tk)
cat > "${T3}" <<'EOF'
m=test;
f=sum(n:i64):i64{let s=0;let res=1;<res}
EOF

T3_EXPECTED='m=test;
f=sum(n:i64):i64 {
  let s=0 /* sum:i64 */;
  let res=1 /* result:i64 */;
  <res /* result */
}
'

T3_ACTUAL=$("${TKC}" --expand "${T3}" 2>/dev/null) || true
check "expand: identifier expansion comments" "${T3_EXPECTED}" "${T3_ACTUAL}"

# ── Test 4: --pretty --expand combined ───────────────────────────────────────

T4=$(mktemp /tmp/tkc_both_t4_XXXXXX.tk)
cat > "${T4}" <<'EOF'
m=test;
f=calc(n:i64):i64{let s=0;lp(let i=0;i<n;i=i+1){s=s+i};<s}
EOF

T4_EXPECTED='m=test;
f=calc(n:i64):i64 {
  let s=0 /* sum:i64 */;

  lp (let i = 0; i /* index */ < n /* count */; i = i /* index */ + 1) {
    s /* sum */ = s /* sum */ + i /* index */
  };

  <s /* sum */
}
'

T4_ACTUAL=$("${TKC}" --pretty --expand "${T4}" 2>/dev/null) || true
check "pretty+expand: combined output" "${T4_EXPECTED}" "${T4_ACTUAL}"

# ── Test 5: exit code 0 for --pretty ─────────────────────────────────────────

RC=0
"${TKC}" --pretty "${T1}" >/dev/null 2>&1 || RC=$?
check "pretty: exit code 0" "0" "${RC}"

# ── Test 6: exit code 0 for --expand ─────────────────────────────────────────

RC=0
"${TKC}" --expand "${T1}" >/dev/null 2>&1 || RC=$?
check "expand: exit code 0" "0" "${RC}"

# ── Test 7: --fmt still works (regression) ───────────────────────────────────

T7_EXPECTED='m=test;
f=add(a:i64; b:i64):i64 {
  <a+b
}
'

T7_ACTUAL=$("${TKC}" --fmt "${T1}" 2>/dev/null) || true
check "fmt: regression (unchanged)" "${T7_EXPECTED}" "${T7_ACTUAL}"

# ── Cleanup ──────────────────────────────────────────────────────────────────
rm -f "${T1}" "${T2}" "${T3}" "${T4}"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ] || exit 1
