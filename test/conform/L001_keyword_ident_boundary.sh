#!/usr/bin/env bash
# L001_keyword_ident_boundary.sh — conformance tests for keyword–identifier boundary behaviour
#
# Verifies that longest-match lexing correctly handles identifiers whose spelling
# begins with a reserved keyword prefix. The lexer must NOT split such an
# alphanumeric run at keyword boundaries.
#
# Normative reference: spec/toke-spec-v02.md §8.9 (Whitespace and Token Separation)
# Story: 44.1.3

set -uo pipefail

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
        echo "    expected: ${expected}"
        echo "    actual:   ${actual}"
        FAIL=$((FAIL + 1))
    fi
}

check_compile_ok() {
    local name="$1" src_file="$2"
    local out
    out=$(mktemp /tmp/tkc_conform_XXXXXX)
    local rc=0
    "${TKC}" --out "${out}" "${src_file}" 2>/tmp/tkc_conform_err || rc=$?
    rm -f "${out}" /tmp/tkc_conform_err
    check "${name} (compiles without error)" "0" "${rc}"
}

check_run_exit() {
    local name="$1" src_file="$2" expected_exit="$3"
    local out
    out=$(mktemp /tmp/tkc_conform_XXXXXX)
    local compile_rc=0
    "${TKC}" --out "${out}" "${src_file}" 2>/tmp/tkc_conform_err || compile_rc=$?
    if [ "${compile_rc}" -ne 0 ]; then
        echo "  FAIL: ${name} (compile failed, rc=${compile_rc})"
        cat /tmp/tkc_conform_err 2>/dev/null | sed 's/^/    /'
        FAIL=$((FAIL + 1))
        rm -f "${out}" /tmp/tkc_conform_err
        return
    fi
    rm -f /tmp/tkc_conform_err
    local run_rc=0
    "${out}" 2>/dev/null || run_rc=$?
    rm -f "${out}"
    check "${name} (exit code)" "${expected_exit}" "${run_rc}"
}

# ── Check tkc exists ──────────────────────────────────────────────────────────
if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

echo "L001: Keyword–identifier boundary conformance tests"
echo "----------------------------------------------------"

# ── Test 1: 'letx' is an identifier, not keyword 'let' + identifier 'x' ──────
#
# 'letx = 5' must be treated as: assign value 5 to identifier 'letx'.
# It is NOT a let-binding. 'letx' is the variable name.

T1=$(mktemp /tmp/tkc_L001_t1_XXXXXX.tk)
cat > "${T1}" <<'EOF'
m=test;
f=main():i64{
  let letx=5;
  <letx
};
EOF
check_run_exit "letx is identifier (not let+x)" "${T1}" "5"
rm -f "${T1}"

# ── Test 2: 'let x' is keyword 'let' + identifier 'x' ───────────────────────
#
# Whitespace between 'let' and 'x' forces two tokens. This is a normal
# let-binding. The result must be 5.

T2=$(mktemp /tmp/tkc_L001_t2_XXXXXX.tk)
cat > "${T2}" <<'EOF'
m=test;
f=main():i64{
  let x=5;
  <x
};
EOF
check_run_exit "let x is let-binding (keyword + ident)" "${T2}" "5"
rm -f "${T2}"

# ── Test 3: 'letmut' is an identifier, not 'let' + 'mut' ────────────────────
#
# 'letmut' must lex as a single identifier token. It is NOT a let-mut binding.

T3=$(mktemp /tmp/tkc_L001_t3_XXXXXX.tk)
cat > "${T3}" <<'EOF'
m=test;
f=main():i64{
  let letmut=7;
  <letmut
};
EOF
check_run_exit "letmut is identifier (not let+mut)" "${T3}" "7"
rm -f "${T3}"

# ── Test 4: Long identifier starting with keyword prefix ─────────────────────
#
# 'mutantninjaturtles' must lex as one identifier regardless of the 'mut'
# prefix. This demonstrates longest-match applies unconditionally.

T4=$(mktemp /tmp/tkc_L001_t4_XXXXXX.tk)
cat > "${T4}" <<'EOF'
m=test;
f=main():i64{
  let mutantninjaturtles=42;
  <mutantninjaturtles
};
EOF
check_run_exit "mutantninjaturtles is one identifier" "${T4}" "42"
rm -f "${T4}"

# ── Test 5: Function named with keyword prefix is valid ───────────────────────
#
# A function declaration 'f=letx(a:i64):i64{<a}' must compile and be callable.
# 'letx' is the function name — a valid identifier. It is not parsed as
# keyword 'let' + identifier 'x'.

T5=$(mktemp /tmp/tkc_L001_t5_XXXXXX.tk)
cat > "${T5}" <<'EOF'
m=test;
f=letx(a:i64):i64{
  <a
};
f=main():i64{
  <letx(9)
};
EOF
check_run_exit "function named letx (keyword prefix) compiles and runs" "${T5}" "9"
rm -f "${T5}"

# ── Test 6: 'let mutantninjaturtles' is a let-binding (keyword + long ident) ─
#
# With whitespace, 'let mutantninjaturtles' must produce keyword 'let' +
# identifier 'mutantninjaturtles'. This is a normal let-binding.

T6=$(mktemp /tmp/tkc_L001_t6_XXXXXX.tk)
cat > "${T6}" <<'EOF'
m=test;
f=main():i64{
  let mutantninjaturtles=13;
  <mutantninjaturtles
};
EOF
check_run_exit "let mutantninjaturtles is let-binding (keyword + long ident)" "${T6}" "13"
rm -f "${T6}"

# ── Summary ───────────────────────────────────────────────────────────────────
echo "----------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ] || exit 1
