#!/usr/bin/env bash
# C734_loop_init_variable_is_mutable.sh — the variable an `lp` init
# introduces is mutable without `mut.` (story 127.283, spec-only).
#
# THE GAP.  docs/spec/semantics.md §2.9 said an assigned binding "must have
# been declared with `let x=mut.`".  Read strictly, that made every
# three-clause loop illegal: `lp(let i=0;i<n;i=i+1)` assigns `i` in its
# step, and `i` is not declared `mut.`.  §4.7 also said the init could be
# written `let x=mut.`, which the parser has never accepted.  The compiler
# was right and the spec was not: types.c treats NODE_LOOP_INIT as mutable,
# and every corpus loop depends on it.  §2.9, §4.7 and toke-spec-v0.4.md §I
# now say so.
#
# This test pins the amended rule against the compiler.  It is green on main
# as well (the amendment changed the spec, not the compiler); its job is to
# go red if either side moves.
#
#   1-step-and-body-assign   the step and a body assignment both assign the
#                            loop variable; compiles, runs, prints 6
#   2-no-mut-in-init         `lp(let i=mut.0;..)` is a parse error, E2002
#   3-immutable-let-in-body  an ordinary immutable `let` assigned inside a
#                            loop body is still E4070 (the loop variable's
#                            mutability does not leak to other bindings)
#
# Story: 127.283

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

# 1. the step and the body both assign the loop variable: i = 0, 2, 4
cat > "${WORK}/p1.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let s=mut.0;
  lp(let i=0;i<5;i=i+1){ s=s+i; i=i+1 };
  io.println("\(s)");
  <0
};
EOF
expect "1-step-and-body-assign-clean" "$(errs "${WORK}/p1.tk")" ""
expect "1-step-and-body-assign-runs"  "$(runs "${WORK}/p1.tk")" "6 "

# 2. `mut.` is not written in a loop init
cat > "${WORK}/p2.tk" <<'EOF'
m=t;
f=main():i64{
  let s=mut.0;
  lp(let i=mut.0;i<5;i=i+1){ s=s+i };
  <s
};
EOF
expect "2-no-mut-in-init" "$(errs "${WORK}/p2.tk" | awk '{print $1}')" "E2002"

# 3. guard: an immutable let assigned in a loop body is still E4070
cat > "${WORK}/p3.tk" <<'EOF'
m=t;
f=main():i64{
  let k=0;
  lp(let i=0;i<3;i=i+1){ k=k+i };
  <k
};
EOF
expect "3-immutable-let-in-body" "$(errs "${WORK}/p3.tk")" "E4070 "

echo "C734: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
