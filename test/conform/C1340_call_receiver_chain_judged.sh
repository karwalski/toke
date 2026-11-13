#!/usr/bin/env bash
# C1340_call_receiver_chain_judged.sh — a method call whose RECEIVER is
# itself a call is checked: the inner call gets every check a call gets
# anywhere else, and the outer call is judged against the inner call's
# established type (story 127.358, the class of 127.318).
#
# THE DEFECT.  infer_impl never inferred a call receiver.  So in
#   `a.concat(b).concat(d)` the inner `a.concat(b)` was never checked (an
#   array has no `.concat`: E4036 when written alone), its result reached
#   the outer call typeless, and recv_value_type() judged only identifiers
#   and literals -- so the outer call was not judged either.  The program
#   passed --check and printed `3 262913 0 0` (expected `4 1 2 3`).  The same
#   hole let `mk().concat(b)` (mk returns @i64) and the inner arity error of
#   `s.slice(1).upper()` through.
#
# THE FIX.  The call site infers a call receiver before judging the call,
#   and recv_value_type() judges a call receiver by that inferred type when
#   it is a proven str, array or map (a scalar-typed result is left alone: a
#   stdlib handle is declared i64 and answers receiver methods).
#
# CASES.
#   1. `a.concat(b).concat(d)` on arrays is E4036, with fix `(a+b)` on the
#      inner call.
#   2. `a.frob(1).bar(2)`: both unknown members reported (was: `bar` only).
#   3. `mk().concat(b)` where mk returns @(i64) is E4036, no fix.
#   4. `s.slice(1).upper()`: the inner call's arity error E4026 is reported.
#   5. valid chains still build and run: a str method chain, a user function
#      returning str used as a receiver, an array slice chain.
#
# Negative control: toke main aace8185's tkc passes cases 1, 3 and 4 with
# --check rc 0 and reports only `bar` in case 2 (measured 2026-11-13).
#
# Story: 127.358
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1340_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C1340: a call receiver is checked and its result judged (127.358)"
echo "---------------------------------------------------------------------------"

# expect_diag <name> <file> <code> <count> — --check fails with exactly
# <count> diagnostics of <code>
expect_diag() {
    "${TKC}" --check "$2" > "$2.out" 2>&1; local rc=$?
    local n; n=$(grep -o "\"error_code\":\"$3\"" "$2.out" | wc -l | tr -d ' ')
    if [ "$rc" -ne 0 ] && [ "$n" = "$4" ]; then pass "$1"
    else fail "$1: rc=$rc, $n x $3 (want $4): $(head -c 400 "$2.out")"; fi
}

# ── 1. the 127.318 array chain ───────────────────────────────────────────
cat > c1.tk <<'TOKE'
m=t;i=io:std.io;
f=main():i64{
  let a=@(1;2);
  let b=@(3);
  let d=@(4);
  let c=a.concat(b).concat(d);
  io.println("\(c.len) \(c.get(0)) \(c.get(1)) \(c.get(2))");
  <0
};
TOKE
expect_diag "array .concat chain is E4036" c1.tk E4036 1
grep -q '"got":"array.concat"' c1.tk.out && grep -q '"fix":"(a+b)"' c1.tk.out \
    && pass "the inner call is the one reported, with fix (a+b)" \
    || fail "wrong got/fix: $(head -c 400 c1.tk.out)"

# ── 2. two unknown members in one chain ──────────────────────────────────
cat > c2.tk <<'TOKE'
m=t;
f=main():i64{
  let a=@(1;2);
  let m2=a.frob(1).bar(2);
  <0
};
TOKE
expect_diag "both members of a.frob(1).bar(2) reported" c2.tk E4036 2
grep -q '"got":"frob"' c2.tk.out && grep -q '"got":"bar"' c2.tk.out \
    && pass "frob and bar both named" || fail "members: $(cat c2.tk.out)"

# ── 3. a user function's array result as the receiver ─────────────────────
cat > c3.tk <<'TOKE'
m=t;i=io:std.io;
f=mk():@(i64){<@(1;2)};
f=main():i64{
  let b=@(3);
  let c=mk().concat(b);
  io.println("\(c.len)");
  <0
};
TOKE
expect_diag "mk().concat(b) with mk:@(i64) is E4036" c3.tk E4036 1
grep -q '"fix"' c3.tk.out && fail "a call receiver got a fix: $(cat c3.tk.out)" \
    || pass "no fix for a call receiver"

# ── 4. the inner call's own arity ────────────────────────────────────────
cat > c4.tk <<'TOKE'
m=t;
f=main():i64{
  let s="hello";
  let t=s.slice(1).upper();
  <0
};
TOKE
expect_diag "inner s.slice(1) arity is E4026" c4.tk E4026 1

# ── 5. valid chains are unchanged ────────────────────────────────────────
cat > c5.tk <<'TOKE'
m=t;i=io:std.io;
f=greet():str{<"hi"};
f=main():i64{
  let p="p";
  let q="q";
  let a=@(1;2;3);
  let c=a.slice(0;2).slice(0;1);
  io.println(p.concat(q).concat("!"));
  io.println(greet().upper());
  io.println("\(c.len) \(c.get(0))");
  <0
};
TOKE
if "${TKC}" --out c5 c5.tk > c5.log 2>&1; then
    out="$(./c5)"; want=$'pq!\nHI\n1 1'
    [ "${out}" = "${want}" ] && pass "valid chains build and print as before" \
        || fail "valid chains printed '${out}', want '${want}'"
else
    fail "valid chains rejected: $(head -c 400 c5.log)"
fi

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
