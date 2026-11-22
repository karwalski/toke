#!/usr/bin/env bash
# C2101_lint_page_shapes.sh — `tkc --lint` reports the four long forms the
# pre-132.15 spec page prescribed, and its fixes are the same program
# (story 131.138).
#
# THE FINDING. `!(a==b)` for `!=`, `!(a>b)` for `<=`, a nested `if` for `&&`
# and `a-a/b*b` for `%` compile clean, and `tkc --lint` reported none of them
# (integration tkc 3.2.0 ee27591c). The corpus sweep had its own regex
# detector (toke-corpus 131.135); every other consumer saw nothing.
#
# THE RULES (hints): negated-equality, negated-comparison, modulo-emulation,
# nested-if-and. A `fix` only where the rewrite is exact in every case:
# integer (or, for `==`, bool) operands by the checker's types; no float
# (NaN: `!=` lowers to `fcmp one`); a nested if only with no `el` on either if
# and no comment in the outer block.
#
# WHAT IS ASSERTED
#   1. Each shape is reported under its rule id; a three-deep if chain is
#      reported once; an `el` on either if is not reported.
#   2. DIFFERENTIAL: `--lint --fix` of an integer program over every sign
#      combination (-7..7, incl. 0 for the comparisons) prints the same as the
#      original at -O0 and -O2.
#   3. TRAPS: `a-a/b*b` and its fix both exit 1 with RT004 for b==0 and for
#      the most negative value by -1.
#   4. WHY floats get no fix: `!(x==y)` and `x!=y` print different results
#      on NaN, and the f64 diagnostics carry no fix.
#   5. `--check` is unchanged: no lint rule id appears there.
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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2101_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C2101: lint rules for the four pre-132.15 spec-page shapes"
echo "-----------------------------------------------------------"

# rules FILE -> "line rule fix|nofix" per finding of the four rules
rules() {
    "${TKC}" --lint --diag-json "$1" 2>/dev/null | python3 -c '
import json, sys
want = {"negated-equality", "negated-comparison", "modulo-emulation", "nested-if-and"}
for l in sys.stdin:
    l = l.strip()
    if not l.startswith("{"): continue
    d = json.loads(l)
    if d.get("rule") in want:
        print(d["pos"]["line"], d["rule"], "fix" if "fix" in d else "nofix")'
}

# ── 1. reporting ────────────────────────────────────────────────────────────
cat > "${WORK}/r.tk" <<'EOF'
m=t;
f=g(a:i64;b:i64;p:bool):i64{
  let n=mut.0;
  if(!(a==b)){n=n+1};
  if(!(a>b)){n=n+1};
  if(!(a<b)){n=n+1};
  n=n+(a-a/b*b);
  if(a>0){if(b>0){if(p){n=n+1}}};
  if(a>0){if(b>0){n=n+1}el{n=n-1}};
  if(a>0){if(b>0){n=n+1}}el{n=n-1};
  if(a>0){n=n+1;if(b>0){n=n+1}};
  if(!(a<=b)){n=n+1};
  <n
};
f=main():i64{<g(1;2;true)};
EOF
got="$(rules "${WORK}/r.tk" | tr '\n' '|')"
want="4 negated-equality fix|5 negated-comparison fix|6 negated-comparison fix|7 modulo-emulation fix|8 nested-if-and fix|"
if [ "${got}" = "${want}" ]; then
    pass "each shape reported once with a fix; el variants, a second statement and !(a<=b) are not"
else
    fail "findings: got '${got}', want '${want}'"
fi

# ── 2. differential over every sign combination ─────────────────────────────
cat > "${WORK}/d.tk" <<'EOF'
m=t;
i=io:std.io;
f=ne(a:i64;b:i64):bool{<!(a==b)};
f=le(a:i64;b:i64):bool{<!(a>b)};
f=ge(a:i64;b:i64):bool{<!(a<b)};
f=md(a:i64;b:i64):i64{<a-a/b*b};
f=md2(a:i64;b:i64):i64{<a-(a/b)*b};
f=both(a:i64;b:i64):i64{
  let n=mut.0;
  if(a>0){if(b<0){n=1}};
  if(a<0||b==0){if(b>=0||a==b){n=n+2}};
  <n
};
f=main():i64{
  lp(let a=0-7;a<8;a=a+1){
    lp(let b=0-7;b<8;b=b+1){
      let m=mut.0;
      let m2=mut.0;
      if(b!=0){m=md(a;b);m2=md2(a;b)};
      io.println("\(a) \(b) \(ne(a;b)) \(le(a;b)) \(ge(a;b)) \(m) \(m2) \(both(a;b))");
    };
  };
  <0
};
EOF
cp "${WORK}/d.tk" "${WORK}/d_fix.tk"
"${TKC}" --lint --fix "${WORK}/d_fix.tk" >/dev/null 2>&1
left="$(rules "${WORK}/d_fix.tk" | wc -l | tr -d ' ')"
if [ "${left}" = "0" ] && ! cmp -s "${WORK}/d.tk" "${WORK}/d_fix.tk"; then
    pass "--lint --fix rewrote every shape (none left)"
else
    fail "after --fix ${left} finding(s) remain: $(rules "${WORK}/d_fix.tk" | tr '\n' '|')"
fi
for o in -O0 -O2; do
    if "${TKC}" "${o}" "${WORK}/d.tk" -o "${WORK}/d_a" >/dev/null 2>&1 &&
       "${TKC}" "${o}" "${WORK}/d_fix.tk" -o "${WORK}/d_b" >/dev/null 2>&1; then
        "${WORK}/d_a" > "${WORK}/a.out" 2>&1; ra=$?
        "${WORK}/d_b" > "${WORK}/b.out" 2>&1; rb=$?
        lines="$(wc -l < "${WORK}/a.out" | tr -d ' ')"
        if [ "${ra}" = 0 ] && [ "${rb}" = 0 ] && [ "${lines}" = 225 ] &&
           cmp -s "${WORK}/a.out" "${WORK}/b.out"; then
            pass "${o}: original and fixed print the same 225 lines"
        else
            fail "${o}: rc ${ra}/${rb}, ${lines} lines, diff: $(diff "${WORK}/a.out" "${WORK}/b.out" | head -4 | tr '\n' '|')"
        fi
    else
        fail "${o}: a program did not build"
    fi
done

# ── 3. traps ────────────────────────────────────────────────────────────────
trap_case() {  # trap_case NAME A B
    cat > "${WORK}/t.tk" <<EOF
m=t;
i=io:std.io;
f=md(a:i64;b:i64):i64{<a-a/b*b};
f=main():i64{io.println("\(md($2;$3))");<0};
EOF
    cp "${WORK}/t.tk" "${WORK}/t_fix.tk"
    "${TKC}" --lint --fix "${WORK}/t_fix.tk" >/dev/null 2>&1
    if ! grep -q 'a%b' "${WORK}/t_fix.tk"; then fail "$1: no a%b after --fix"; return; fi
    "${TKC}" "${WORK}/t.tk" -o "${WORK}/t_a" >/dev/null 2>&1
    "${TKC}" "${WORK}/t_fix.tk" -o "${WORK}/t_b" >/dev/null 2>&1
    "${WORK}/t_a" > "${WORK}/ta.out" 2> "${WORK}/ta.err"; ra=$?
    "${WORK}/t_b" > "${WORK}/tb.out" 2> "${WORK}/tb.err"; rb=$?
    if [ "${ra}" = 1 ] && [ "${rb}" = 1 ] && grep -q '^RT004' "${WORK}/ta.err" &&
       grep -q '^RT004' "${WORK}/tb.err" && cmp -s "${WORK}/ta.out" "${WORK}/tb.out"; then
        pass "$1: both trap RT004, exit 1, same stdout"
    else
        fail "$1: rc ${ra}/${rb}; $(head -1 "${WORK}/ta.err") / $(head -1 "${WORK}/tb.err")"
    fi
}
trap_case "b==0" 7 0
trap_case "MIN/-1" "0-9223372036854775807-1" "0-1"

# ── 4. floats: why there is no fix ──────────────────────────────────────────
cat > "${WORK}/f.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let z=0.0;
  let x=z/z;
  let y=1.0;
  io.println("\(!(x==y)) \(x!=y) \(!(x>y)) \(x<=y)");
  if(!(x==y)){io.println("ne")};
  if(!(x>y)){io.println("le")};
  <0
};
EOF
if "${TKC}" "${WORK}/f.tk" -o "${WORK}/f" >/dev/null 2>&1; then
    fo="$("${WORK}/f" 2>&1 | head -1)"
    if [ "${fo}" = "true false true false" ]; then
        pass "NaN: !(x==y)/x!=y and !(x>y)/x<=y differ (${fo})"
    else
        fail "NaN line: '${fo}'"
    fi
else
    fail "float program did not build"
fi
got="$(rules "${WORK}/f.tk" | tr '\n' '|')"
if [ "${got}" = "7 negated-equality nofix|7 negated-comparison nofix|8 negated-equality nofix|9 negated-comparison nofix|" ]; then
    pass "f64 operands (in an interpolation and in a condition): reported, no fix"
else
    fail "f64 findings: '${got}'"
fi

# ── 5. --check unchanged ────────────────────────────────────────────────────
if "${TKC}" --check "${WORK}/r.tk" >"${WORK}/chk.out" 2>&1 &&
   ! grep -qE 'negated-|modulo-emulation|nested-if-and' "${WORK}/chk.out"; then
    pass "--check: clean, no lint rule ids"
else
    fail "--check: $(head -c 300 "${WORK}/chk.out")"
fi

echo ""
echo "C2101: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
