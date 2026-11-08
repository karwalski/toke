#!/usr/bin/env bash
# C921_concat_chain_fix_str_only.sh — the dormant string-concat-chain --fix
# (TKC_LINT_CONCAT_FIX=1) rewrites only chains the checker proves all-str,
# and the rewritten program builds and prints what the original printed
# (story 131.110).
#
# THE DEFECT.  The fix was AST-only: any `.concat` method and any import
#   alias's `concat` matched, and any identifier was a leaf.  An ARRAY chain
#   `a.concat(b).concat(d)` became `"\(a)\(b)\(d)"`, which is E4032 (cannot
#   interpolate a composite), so `--lint --fix` broke a program that built.
#
# THE FIX.  The fix is offered only when the program type-checked cleanly
#   (LintOptions.typed), every call in the chain is module style through an
#   alias that resolves to std.str and is typed str, every identifier leaf is
#   typed exactly str, and the rewritten program re-checks cleanly
#   (LintOptions.recheck).  Method-style chains keep the warning without a
#   fix: the checker types neither their receiver nor their result.
#
# WHAT IS ASSERTED (each program built and run before and after --fix).
#   1. str program: the module-style chains are rewritten to interpolation,
#      the method-style chain is left alone, and the output is unchanged.
#   2. array program: --fix leaves the source byte-identical, and it still
#      builds and prints what it printed (before the fix: rewritten, then
#      E4032).
#   3. without TKC_LINT_CONCAT_FIX the rule stays dormant: no chain rewritten.
#
# Negative control: integrate/z-wave-1108 f397c9a0's tkc rewrites part 2's
# chain to "\(a)\(b)\(d)" and the result fails --check with E4032
# (measured 2026-11-09).
#
# Story: 131.110
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c921_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C921: string-concat-chain --fix only rewrites all-str std.str chains (131.110)"
echo "---------------------------------------------------------------------------"

# build_run <src> <bin> -> prints the program's stdout; returns 1 on failure
build_run() {
    "${TKC}" --out "$2" "$1" > "$2.log" 2>&1 || { head -3 "$2.log" | sed 's/^/        /'; return 1; }
    "./$2"
}

# ── 1. a str program ─────────────────────────────────────────────────────
cat > s.tk <<'TOKE'
m=t;i=io:std.io;i=s:std.str;
f=label(a:str;b:str):str{
  <s.concat("x=";s.concat(a;s.concat(", y=";b)))
};
f=main():i64{
  let p="p";
  let q="q";
  io.println(label(p;q));
  io.println(s.concat(s.concat(p;"\"");q));
  io.println(p.concat(q).concat("!"));
  io.println(label(q;p));
  <0
};
TOKE
before="$(build_run s.tk s0)" || fail "str program did not build before --fix"
cp s.tk s_fix.tk
TKC_LINT_CONCAT_FIX=1 "${TKC}" --lint --fix s_fix.tk > fix1.log 2>&1
if grep -qF '<"x=\(a), y=\(b)"' s_fix.tk && grep -qF 'io.println("\(p)\"\(q)");' s_fix.tk; then
    pass "module-style std.str chains rewritten to interpolation"
else
    fail "module-style chains not rewritten:"; sed 's/^/        /' s_fix.tk
fi
if grep -qF 'p.concat(q).concat("!")' s_fix.tk; then
    pass "method-style chain left as written (receiver untyped)"
else
    fail "method-style chain was rewritten"; sed 's/^/        /' s_fix.tk
fi
after="$(build_run s_fix.tk s1)" || fail "str program did not build after --fix"
want=$'x=p, y=q\np"q\npq!\nx=q, y=p'
if [ "${before}" = "${want}" ] && [ "${after}" = "${want}" ]; then
    pass "str program prints the same before and after --fix"
else
    fail "output: before '${before}' after '${after}' want '${want}'"
fi

# ── 2. an array program ──────────────────────────────────────────────────
cat > a.tk <<'TOKE'
m=t;i=io:std.io;
f=main():i64{
  let a=@(1;2);
  let b=@(3);
  let d=@(4);
  let c=a.concat(b).concat(d);
  io.println("\(c.len) \(a.len) \(b.len) \(d.len)");
  <0
};
TOKE
before="$(build_run a.tk a0)" || fail "array program did not build before --fix"
cp a.tk a_fix.tk
TKC_LINT_CONCAT_FIX=1 "${TKC}" --lint --fix a_fix.tk > fix2.log 2>&1
grep -q '"string-concat-chain"' fix2.log && pass "array chain still flagged" \
    || fail "array chain not flagged: $(head -c 300 fix2.log)"
cmp -s a.tk a_fix.tk && pass "--fix leaves the array program byte-identical" \
    || { fail "--fix changed the array program:"; diff a.tk a_fix.tk | sed 's/^/        /'; }
after="$(build_run a_fix.tk a1)" || fail "array program did not build after --fix"
# Only "unchanged by --fix" is asserted: the chain's own value is wrong today
# (c.len is 3, not 4 -- story 127.318, a method-style array concat chain
# passes the checker and is mis-lowered), which is not this rule's concern.
[ -n "${before}" ] && [ "${before}" = "${after}" ] \
    && pass "array program builds and prints the same before and after (${after})" \
    || fail "array output: before '${before}' after '${after}'"

# ── 3. dormant by default ────────────────────────────────────────────────
cp s.tk s_off.tk
env -u TKC_LINT_CONCAT_FIX "${TKC}" --lint --fix s_off.tk > fix3.log 2>&1
if grep -qF 's.concat("x=";s.concat(a;s.concat(", y=";b)))' s_off.tk &&
   grep -qF 's.concat(s.concat(p;"\"");q)' s_off.tk; then
    pass "without TKC_LINT_CONCAT_FIX no chain is rewritten"
else
    fail "a chain was rewritten with the gate off"; sed 's/^/        /' s_off.tk
fi

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
