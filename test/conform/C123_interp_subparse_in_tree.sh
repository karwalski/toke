#!/usr/bin/env bash
# C123_interp_subparse_in_tree.sh — a name used inside `\(..)` resolves as the
# same name written outside the string does (story 127.221, item 2).
#
# THE DEFECT. check_interp_composites re-parses each interpolation and infers
# the sub-expression; its nodes are re-homed onto the real source but are not
# in the function tree.  Every scope test built on node_contains() therefore
# answered "not here" for them: find_binding_before had been patched (C054),
# the rest had not.
#   * an `mt` arm binding (arm_bind_lookup): `$ok:t io.println("\(t.upper())")`
#     with a later `let t=@("x")` in the function typed `t` as that array — a
#     false E4036 "no receiver method 'upper' on a '@(str)'";
#   * a closure's own `let` (local_in_force): `let sum=fn..` inside a closure,
#     then `"\(sum(@(1;2)))"` in the same closure, was checked against a
#     module `sum(a:$str)` — a false E4031.
# Both now stand the string literal in for the sub-parse node (in_tree_use,
# types.c), the anchor find_binding_before already used in spirit.
#
# Negative control: TKC built from main at 8f4a843 — cases 1 and 2 red.
#
# Story: 127.221

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

# 1. an `mt` arm binding used inside an interpolation in the arm
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
t=$perr{$bad:str};
f=pr(x:str):str!$perr{<x.trim()};
f=main():i64{let r=pr(" 12 ");mt r{$ok:t io.println("ok \(t.len) \(t.upper())");$err:e io.println("err")};let t=@("x";"y");io.println(t.get(0));<0};
EOF
expect "1-arm-binding-in-interp-checks" "$(errs "${WORK}/p1.tk")" ""
expect "1-arm-binding-in-interp-runs"   "$(runs "${WORK}/p1.tk")" "ok 2 12 x "

# 2. a closure's own `let` used inside an interpolation in the closure
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=sum(a:$str):$str{<a};
f=main():i64{
  let g=fn(b:i64):i64{let sum=fn(x:@(i64)):i64{<3};io.println("n=\(sum(@(1;2)))");<0};
  <0
};
EOF
expect "2-closure-let-in-interp-checks" "$(errs "${WORK}/p2.tk")" ""

# 3. positive guard: outside the closure the module function is checked
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
f=sum(a:$str):$str{<a};
f=main():i64{
  let g=fn(b:i64):i64{let sum=fn(x:@(i64)):i64{<3};<0};
  io.println("n=\(sum(@(1;2)))");
  <0
};
EOF
expect "3-module-fn-outside-still-checked" "$(errs "${WORK}/p3.tk")" "E4031 "

echo "C123: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
