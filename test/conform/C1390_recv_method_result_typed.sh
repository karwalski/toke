#!/usr/bin/env bash
# C1390_recv_method_result_typed.sh — a kind-preserving receiver method's
# RESULT carries the receiver's type, so a method called on it -- in a chain
# or through a `let` bound to it -- is judged (story 127.364).
#
# THE DEFECT.  infer_impl typed every receiver-method call TY_UNKNOWN.  After
#   127.358 a call receiver is judged by its inferred type, but that type was
#   unknown, so `let c=a.slice(0;2).concat(b)` on arrays passed --check and
#   printed `2` (an array has no `.concat`: E4036 when written on `a`), and
#   `let x=a.slice(0;2); x.concat(b)` was the same.
#
# THE FIX.  Where the receiver's type is established and the member lowers on
#   that kind, the result is typed by what the lowering returns:
#     array .slice                                   -> the receiver's @T
#     str .slice/.concat/.trim/.upper/.lower/.replace/.substr -> str
#   in place and through an immutable `let`.  127.371 (C1450) adds more
#   members and a `mut` binding whose every store agrees.
#
# CASES.
#   1. `a.slice(0;2).concat(b)` on arrays is E4036 (got array.concat), no fix.
#   2. `let x=a.slice(0;2); x.concat(b)` is E4036, fix `(x+b)` (D360).
#   3. `s.trim().upper().push(1)` on a str is E4036 (got str.push).
#   4. `let t=s.replace("a";"b"); t.keys()` is E4036 (got str.keys).
#   5. `a.slice(0;2).slice(0;1).concat(b)` -- the type survives a chain.
#   6. valid chains build and print as before.
#
# Negative control: integrate/z-wave-1113 (e1155b19) passes cases 1-5 with
# --check rc 0 (measured 2026-11-13).
#
# Story: 127.364
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1390_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C1390: a receiver method's result is typed and the next call judged (127.364)"
echo "---------------------------------------------------------------------------"

# expect_diag <name> <file> <code> <count> <got> — --check fails with exactly
# <count> diagnostics of <code>, and one of them has "got":<got>
expect_diag() {
    "${TKC}" --check "$2" > "$2.out" 2>&1; local rc=$?
    local n; n=$(grep -o "\"error_code\":\"$3\"" "$2.out" | wc -l | tr -d ' ')
    if [ "$rc" -ne 0 ] && [ "$n" = "$4" ] && grep -q "\"got\":\"$5\"" "$2.out"; then pass "$1"
    else fail "$1: rc=$rc, $n x $3 (want $4, got $5): $(head -c 400 "$2.out")"; fi
}

# ── 1. the story's chain ─────────────────────────────────────────────────
cat > c1.tk <<'TOKE'
m=t;i=io:std.io;
f=main():i64{
  let a=@(1;2;3);
  let b=@(9);
  let c=a.slice(0;2).concat(b);
  io.println("\(c.len)");
  <0
};
TOKE
expect_diag "a.slice(0;2).concat(b) on arrays is E4036" c1.tk E4036 1 "array.concat"
grep -q '"fix"' c1.tk.out && fail "a call receiver got a fix: $(cat c1.tk.out)" \
    || pass "no fix for a call receiver"

# ── 2. the let-bound form ────────────────────────────────────────────────
cat > c2.tk <<'TOKE'
m=t;i=io:std.io;
f=main():i64{
  let a=@(1;2;3);
  let b=@(9);
  let x=a.slice(0;2);
  let c=x.concat(b);
  io.println("\(c.len)");
  <0
};
TOKE
expect_diag "let x=a.slice(0;2); x.concat(b) is E4036" c2.tk E4036 1 "array.concat"
grep -q '"fix":"(x+b)"' c2.tk.out && pass "the let-bound form gets fix (x+b)" \
    || fail "missing fix (x+b): $(head -c 400 c2.tk.out)"

# ── 3. a str chain into an array-only member ─────────────────────────────
cat > c3.tk <<'TOKE'
m=t;
f=main():i64{
  let s=" ab ";
  let u=s.trim().upper().push(1);
  <0
};
TOKE
expect_diag "s.trim().upper().push(1) is E4036" c3.tk E4036 1 "str.push"

# ── 4. a let-bound str result into a map-only member ─────────────────────
cat > c4.tk <<'TOKE'
m=t;
f=main():i64{
  let s="abc";
  let t=s.replace("a";"b");
  let k=t.keys();
  <0
};
TOKE
expect_diag "let t=s.replace(..); t.keys() is E4036" c4.tk E4036 1 "str.keys"

# ── 5. the type survives a longer chain ──────────────────────────────────
cat > c5.tk <<'TOKE'
m=t;
f=main():i64{
  let a=@(1;2;3);
  let b=@(9);
  let c=a.slice(0;2).slice(0;1).concat(b);
  <0
};
TOKE
expect_diag "a.slice(..).slice(..).concat(b) is E4036" c5.tk E4036 1 "array.concat"

# ── 6. valid chains are unchanged ────────────────────────────────────────
cat > c6.tk <<'TOKE'
m=t;i=io:std.io;
f=main():i64{
  let s="  Hello ";
  let t=mt s.trim().upper().slice(0;3){$ok:v v;$err:e ""};
  let u=t.concat("!").lower().replace("h";"j").substr(0;3);
  let a=@(1;2;3;4);
  let v=a.slice(1;3).slice(0;1);
  let w=a.slice(0;2);
  io.println("\(t) \(u) \(v.len) \(v.get(0)) \(w.len) \(w.get(1))");
  io.println(u.upper());
  <0
};
TOKE
if "${TKC}" --out c6 c6.tk > c6.log 2>&1; then
    out="$(./c6)"; want=$'HEL jel 1 2 2 2\nJEL'
    [ "${out}" = "${want}" ] && pass "valid chains build and print as before" \
        || fail "valid chains printed '${out}', want '${want}'"
else
    fail "valid chains rejected: $(head -c 400 c6.log)"
fi

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
