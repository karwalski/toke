#!/usr/bin/env bash
# F001_format_basic.sh — test canonical formatter (toke fmt)
#
# Verifies:
#   1. Messy input is reformatted to canonical form
#   2. Idempotency: formatting the output again produces identical text
#   3. Exit code 0 on valid input, 1 on invalid input
#
# Story: 10.8.1

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

echo "F001: Canonical formatter basic tests"
echo "--------------------------------------"

# ── Test 1: Messy input reformatted to canonical form ─────────────────────────

MESSY=$(mktemp /tmp/tkc_fmt_messy_XXXXXX.tk)
cat > "${MESSY}" <<'EOF'
M=example;
I=str:std.str;

T=Point{x:i64;y:i64};

F=add( a : i64 ;  b : i64 ) : i64 {  rt a+b  }

F=main():void{let r=add(1;2);rt r}
EOF

EXPECTED='M=example;
I=str:std.str;

T=Point {
  x:i64;
  y:i64
};

F=add(a:i64; b:i64):i64 {
  rt a+b
};

F=main():void {
  let r=add(1; 2);
  rt r
}
'

ACTUAL=$("${TKC}" --fmt "${MESSY}" 2>/dev/null) || true
check "messy input reformatted" "${EXPECTED}" "${ACTUAL}"

# ── Test 2: Idempotency ──────────────────────────────────────────────────────

CANONICAL=$(mktemp /tmp/tkc_fmt_canon_XXXXXX.tk)
echo -n "${ACTUAL}" > "${CANONICAL}"
SECOND=$("${TKC}" --fmt "${CANONICAL}" 2>/dev/null) || true
check "idempotent (fmt|fmt = fmt)" "${ACTUAL}" "${SECOND}"

# ── Test 3: Exit code 0 on valid input ───────────────────────────────────────

VALID=$(mktemp /tmp/tkc_fmt_valid_XXXXXX.tk)
cat > "${VALID}" <<'EOF'
M=test;
F=noop():void{rt}
EOF

RC=0
"${TKC}" --fmt "${VALID}" >/dev/null 2>&1 || RC=$?
check "exit 0 on valid input" "0" "${RC}"

# ── Test 4: Exit code 1 on parse error ───────────────────────────────────────

INVALID=$(mktemp /tmp/tkc_fmt_bad_XXXXXX.tk)
cat > "${INVALID}" <<'EOF'
this is not valid toke
EOF

RC=0
"${TKC}" --fmt "${INVALID}" >/dev/null 2>&1 || RC=$?
if [ "${RC}" -ne 0 ]; then
    echo "  PASS: exit non-zero on invalid input (got ${RC})"
    PASS=$((PASS + 1))
else
    echo "  FAIL: exit non-zero on invalid input (got 0)"
    FAIL=$((FAIL + 1))
fi

# ── Test 5: If/else formatting ───────────────────────────────────────────────

IFTEST=$(mktemp /tmp/tkc_fmt_if_XXXXXX.tk)
cat > "${IFTEST}" <<'EOF'
M=test;
F=abs(x:i64):i64{if(x<0){rt 0-x}el{rt x}}
EOF

IF_EXPECTED='M=test;
F=abs(x:i64):i64 {
  if (x<0) {
    rt 0-x
  } el {
    rt x
  }
}
'

IF_ACTUAL=$("${TKC}" --fmt "${IFTEST}" 2>/dev/null) || true
check "if/else formatting" "${IF_EXPECTED}" "${IF_ACTUAL}"

# ── Test 6: Loop formatting ─────────────────────────────────────────────────

LPTEST=$(mktemp /tmp/tkc_fmt_lp_XXXXXX.tk)
cat > "${LPTEST}" <<'EOF'
M=test;
F=sum(n:i64):i64{let s=0;lp(let i=0;i<n;i=i+1){s=s+i};rt s}
EOF

LP_EXPECTED='M=test;
F=sum(n:i64):i64 {
  let s=0;
  lp (let i=0; i<n; i=i+1) {
    s=s+i
  };
  rt s
}
'

LP_ACTUAL=$("${TKC}" --fmt "${LPTEST}" 2>/dev/null) || true
check "loop formatting" "${LP_EXPECTED}" "${LP_ACTUAL}"

# ── Cleanup ──────────────────────────────────────────────────────────────────
rm -f "${MESSY}" "${CANONICAL}" "${VALID}" "${INVALID}" "${IFTEST}" "${LPTEST}"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ] || exit 1
