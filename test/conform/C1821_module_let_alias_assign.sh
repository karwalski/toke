#!/usr/bin/env bash
# C1821_module_let_alias_assign.sh -- an assignment to a module-level
# immutable `let` or to an import alias is E4070 (story 127.465).
#
# THE DEFECT.  The assignment check looked for a local binder and, when it
# found none, knew only module constants (127.310).  So `let g=5;` at module
# level and `g=0;` in a function checked clean and silently overwrote the
# immutable binding (semantics.md §4.2: DECL_LET is not mutable), and
# `io=0;` with `i=io:std.io;` checked clean and then failed in codegen with
# E9003 (clang: use of undefined value '%io'), reported at the function, not
# the assignment.  127.289 found the gap through the bare `lp` init, which is
# checked as this statement is (C1820 cases 15-16).
#
# THE RULE.  Both are E4070 at the assignment.  A module-level `let` carries
# the local `let` fix, "change 'let g=' to 'let g=mut.' to make it mutable":
# `let g=mut.<v>` is a valid module-level binding for every initialiser a
# module `let` takes, and --migrate makes the same rewrite.  It does not on
# the step of a loop whose bare init assigns the same binding (127.289: the
# init's two repairs run differently).  An import alias has no mutable
# spelling, so no fix.
#
#   1  module-level let, `g=0;` -> E4070 at the assignment, the exact fix,
#      --check fails
#   2  applying that fix: the program checks clean and prints the new value
#   3  import alias, `io=0;` -> E4070 (got "import alias"), no fix, --check
#      fails, and a build reports E4070, not E9003
#   4  a closure body assigning a module-level let -> E4070
#   5  a module-level let assigned in a loop body keeps its fix; the step of
#      a loop whose bare init assigns it has none
#   6  --migrate rewrites an assigned module-level let to `let g=mut.` and
#      the result checks clean
#   7  guard: a module `mut` global is assigned (runtime)
#   8  guard: a local `let g=mut.` shadowing a module `let g` is assigned; the
#      module binding is untouched (runtime)
#   9  guard: reading a module-level let and calling through an alias are
#      clean
#
# Negative control: TKC built from 685aa852 (main) -- 8 of 17 red: every
# check of 1, 3, 4 and 5 (they check clean, and the alias fails in codegen
# with E9003) and 6's rewrite (--migrate declines a file that checks clean);
# 2, 6's clean check and 7-9 pass on both.
#
# Story: 127.465

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
# error codes in order, space-separated
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
# one line per error: CODE@line:col|got|fix (fix "-" when absent)
diags() { "${TKC}" --check "$1" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    l = l.strip()
    if not l.startswith("{"): continue
    d = json.loads(l)
    if d.get("severity") != "error": continue
    print("%s@%d:%d|%s|%s" % (d["error_code"], d["pos"]["line"], d["pos"]["col"],
                              d.get("got", ""), d.get("fix", "-")))'; }
check_rc() { if "${TKC}" --check "$1" >/dev/null 2>&1; then echo passes; else echo fails; fi; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
FIX="change 'let g=' to 'let g=mut.' to make it mutable"

# 1. module-level immutable let assigned by a statement
cat > "${WORK}/p1.tk" <<'EOF'
m=t;
i=io:std.io;
let g=5;
f=main():i64{
  g=0;
  io.println("\(g)");
  <0
};
EOF
expect "1-module-let-E4070-fix" "$(diags "${WORK}/p1.tk")" "E4070@5:3|immutable binding|${FIX}"
expect "1-module-let-check-fails" "$(check_rc "${WORK}/p1.tk")" "fails"

# 2. the fix, applied as it reads, gives a program that checks and runs
sed 's/^let g=/let g=mut./' "${WORK}/p1.tk" > "${WORK}/p2.tk"
expect "2-fix-applied-clean" "$(errs "${WORK}/p2.tk")" ""
expect "2-fix-applied-runs"  "$(runs "${WORK}/p2.tk")" "0 "

# 3. import alias assigned by a statement
cat > "${WORK}/p3.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  io=0;
  <0
};
EOF
expect "3-alias-E4070-no-fix" "$(diags "${WORK}/p3.tk")" "E4070@4:3|import alias|-"
expect "3-alias-check-fails"  "$(check_rc "${WORK}/p3.tk")" "fails"
b3="$("${TKC}" "${WORK}/p3.tk" -o "${WORK}/b3" 2>&1 | grep -o '"error_code":"E[0-9]*"' | head -1)"
expect "3-alias-build-E4070-not-E9003" "${b3}" '"error_code":"E4070"'

# 4. a closure body assigning a module-level let
cat > "${WORK}/p4.tk" <<'EOF'
m=t;
let g=5;
f=main():i64{
  let c=fn():i64{ g=1; <g };
  <c()
};
EOF
expect "4-closure-module-let-E4070" "$(errs "${WORK}/p4.tk")" "E4070 "

# 5. loop body keeps the fix; the step of a bare-init loop over g has none
cat > "${WORK}/p5.tk" <<'EOF'
m=t;
let g=5;
f=main():i64{
  let j=mut.0;
  lp(let k=0;k<2;k=k+1){ g=k };
  lp(g=0;g<3;g=g+1){ j=j+1 };
  <j
};
EOF
expect "5-body-fix-init-and-step-none" "$(diags "${WORK}/p5.tk")" "E4070@5:26|immutable binding|${FIX}
E4070@6:6|immutable binding|-
E4070@6:14|immutable binding|-"

# 6. --migrate makes the same rewrite; the result checks clean
"${TKC}" --migrate "${WORK}/p1.tk" > "${WORK}/p6.tk" 2>/dev/null
expect "6-migrate-adds-mut" "$(grep -c '^let g=mut\.5;' "${WORK}/p6.tk")" "1"
expect "6-migrated-clean"   "$(errs "${WORK}/p6.tk")" ""

# 7. guard: a module mut global is assigned
cat > "${WORK}/p7.tk" <<'EOF'
m=t;
i=io:std.io;
let g=mut."a";
f=main():i64{
  g="b";
  io.println(g);
  <0
};
EOF
expect "7-mut-global-clean" "$(errs "${WORK}/p7.tk")" ""
expect "7-mut-global-runs"  "$(runs "${WORK}/p7.tk")" "b "

# 8. guard: a local mut shadowing a module let is the one assigned
cat > "${WORK}/p8.tk" <<'EOF'
m=t;
i=io:std.io;
let g=5;
f=other():i64{ <g };
f=main():i64{
  let g=mut.1;
  g=3;
  io.println("\(g) \(other())");
  <0
};
EOF
expect "8-local-shadow-clean" "$(errs "${WORK}/p8.tk")" ""
expect "8-local-shadow-runs"  "$(runs "${WORK}/p8.tk")" "3 5 "

# 9. guard: reading a module let, calling through an alias
cat > "${WORK}/p9.tk" <<'EOF'
m=t;
i=io:std.io;
let g=5;
f=main():i64{
  io.println("\(g+1)");
  <0
};
EOF
expect "9-read-clean" "$(errs "${WORK}/p9.tk")" ""
expect "9-read-runs"  "$(runs "${WORK}/p9.tk")" "6 "

echo "C1821: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
