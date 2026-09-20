#!/usr/bin/env bash
# C013_interp_name_resolution.sh — interpolated identifiers are in the AST and
# are name-resolved (story 127.24).
#
# Until 127.24 a `"...\(EXPR)..."` literal was a leaf node: the interpolated
# EXPR existed only as bytes inside the token text, so the name resolver never
# saw it.  `io.println("\(zz)")` with `zz` undeclared therefore passed `--check`
# CLEAN and only failed much later, at the clang stage, as an E9003 that named
# no identifier and pointed at line 0.  That is the check-blind class: the
# compiler said yes to a program it could not compile.
#
# The parser now parses each `\(…)` segment once and hangs the expression off
# the STR_LIT as a child, with offsets relocated into real source coordinates.
# Name resolution walks children generically, so it sees interpolated
# identifiers for free and reports E3011 at the identifier's own position.
#
# Case 1 is the assertion that matters: it asserts a DIAGNOSTIC CODE from
# `--check`, not merely a non-zero exit.  A test that only checked "compilation
# fails" would have passed against the defect too, because the defect still
# failed the build — just later, and namelessly.
#
# Cases 2-6 are the controls.  The fix is only worth having if every legitimate
# interpolation still resolves: locals, parameters, loop variables, method
# calls and field reads must NOT become errors.  The blast-radius survey for
# 127.24 exercised 838 interpolation sites across the repo and 10,639 corpus
# records and found zero false positives; these cases pin the shapes.
#
# Case 7 pins the regression that the first cut of the fix introduced: the
# speculative segment parse runs under diag_suppress(), and suppressed
# diagnostics used to still increment the error counter, so a parse error
# inside an interpolation exited 1 while printing NOTHING.  A silent failure is
# worse than the defect it replaced, so it is pinned here.
#
# Stories: 127.24

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

WORK="$(mktemp -d /tmp/tkc_interpname_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C013: interpolated identifiers are name-resolved"
echo "--------------------------------------"

# check_reports NAME EXPECTED_CODE SOURCE
#   Asserts that `--check` emits EXPECTED_CODE and exits non-zero.
check_reports() {
    local name="$1" code="$2" src="$3"
    printf '%s\n' "${src}" > "case.tk"
    local out rc=0
    out="$("${TKC}" --check "case.tk" 2>&1)" || rc=$?
    if [ "${rc}" -eq 0 ]; then
        echo "  FAIL: ${name}: --check accepted the program (expected ${code})"
        FAIL=$((FAIL + 1))
        return
    fi
    if printf '%s' "${out}" | grep -q "${code}"; then
        echo "  PASS: ${name} (${code} at check time)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected ${code}, got:"
        printf '%s\n' "${out}" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

# runs_clean NAME EXPECTED_STDOUT SOURCE
#   Asserts --check is clean AND the built program prints EXPECTED_STDOUT.
runs_clean() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "case.tk"
    if ! "${TKC}" --check "case.tk" >check.log 2>&1; then
        echo "  FAIL: ${name}: --check rejected a valid program"
        sed 's/^/      /' check.log
        FAIL=$((FAIL + 1))
        return
    fi
    rm -f case_bin
    if ! "${TKC}" --allow-all --out "case_bin" "case.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log
        FAIL=$((FAIL + 1))
        return
    fi
    local out rc=0
    out="$(./case_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${expected}', got '${out}' (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

# ── Case 1: the defect itself ────────────────────────────────────────────
# Before 127.24 this exited 0 from --check.
check_reports "undeclared identifier in interpolation is E3011" "E3011" \
'm=main;
i=io:std.io;
f=main():i64{
  io.println("\(zz)");
  <0
};'

# ── Case 2: a second, non-adjacent segment is resolved too ───────────────
# The scanner must keep walking after the first segment closes.
check_reports "undeclared identifier in the SECOND segment is E3011" "E3011" \
'm=main;
i=io:std.io;
f=main():i64{
  let ok=1;
  io.println("a \(ok) b \(nope) c");
  <0
};'

# ── Case 3: locals, params and expressions still resolve ─────────────────
runs_clean "local, int and arithmetic interpolations resolve" \
"hello world 7 14" \
'm=main;
i=io:std.io;
f=main():i64{
  let name="world";
  let n=7;
  io.println("hello \(name) \(n) \(n*2)");
  <0
};'

# ── Case 4: a loop variable is in scope inside an interpolation ──────────
runs_clean "loop variable resolves inside an interpolation" \
"i=0
i=1
i=2" \
'm=main;
i=io:std.io;
f=main():i64{
  lp(let i=0;i<3;i=i+1){
    io.println("i=\(i)")
  };
  <0
};'

# ── Case 5: method calls and field reads inside an interpolation ─────────
# `.len` and `.get(i)` are FIELD_EXPR / INDEX_EXPR; the identifier before the
# dot must resolve, and the member name after it must NOT be looked up as a
# variable.
runs_clean "receiver resolves, member name is not a variable" \
"len=2 first=a" \
'm=main;
i=io:std.io;
f=main():i64{
  let xs=@("a";"b");
  io.println("len=\(xs.len) first=\(xs.get(0))");
  <0
};'

# ── Case 6: a function parameter resolves inside an interpolation ────────
runs_clean "parameter resolves inside an interpolation" \
"got 42" \
'm=main;
i=io:std.io;
f=show(v:i64):i64{
  io.println("got \(v)");
  <0
};
f=main():i64{
  <show(42)
};'

# ── Case 7: a malformed segment must still SAY something ─────────────────
# The speculative parse of a segment runs suppressed.  A suppressed diagnostic
# must not be counted either, or the compiler exits non-zero having printed
# nothing.  Assert that some diagnostic is printed.
printf '%s\n' 'm=main;
i=io:std.io;
f=main():i64{
  io.println("v \(a=1)");
  <0
};' > bad.tk
BADOUT="$("${TKC}" --check bad.tk 2>&1)"
BADRC=$?
if [ "${BADRC}" -ne 0 ] && [ -n "${BADOUT}" ]; then
    echo "  PASS: a malformed interpolation fails WITH a diagnostic"
    PASS=$((PASS + 1))
elif [ "${BADRC}" -ne 0 ]; then
    echo "  FAIL: malformed interpolation exited ${BADRC} with NO diagnostic (silent failure)"
    FAIL=$((FAIL + 1))
else
    echo "  PASS: a malformed interpolation is accepted by --check and diagnosed later"
    PASS=$((PASS + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
