#!/usr/bin/env bash
# C1820_lp_bare_init_assigns.sh — a bare `lp(i=0;..)` init assigns the
# binding of `i` in force; only `lp(let i=0;..)` declares a loop variable
# (story 127.289).
#
# THE DEFECT.  A bare init made a fresh loop variable whatever was in scope.
# With an outer `let i=mut.7`, `lp(i=0;i<5;i=i+1){..}` left the outer i at
# 7 after the loop; with an outer immutable `let i=7`, or a parameter `i`,
# it compiled clean with no E4070.  A reader -- and a model -- reads `i=0`
# as an assignment, and v0.3 §11.10 says the init is "a binding or
# assignment".
#
# THE RULE (owner decision 2026-10-01 §2.18 (a); toke-spec-v0.4.md §K;
# semantics.md §2.9, §4.7).  A bare init is the assignment `i=0` to the
# binding in force: the step and the body assign that same binding and its
# last value is visible after the loop.  Over an immutable `let`, a
# parameter or an `mt` arm binding it is E4070, with no `fix` (two repairs
# that run differently are plausible: `let i=mut.` or `lp(let i=..`); with
# no binding in force it is E3011, at the init.  `lp(let i=0;..)` still
# declares a fresh variable, scoped to the loop.
#
#   1  outer mut, bare init assigns it; the value after the loop is the
#      final counter (runtime)
#   2  outer immutable let -> E4070 at the init, no fix there
#   3  parameter -> E4070 at the init, no fix
#   4  match-arm binding -> E4070 at the init
#   5  no binding in force -> E3011 at the init (col 6), reported once
#   6  module `mut` global -> the loop assigns the global (runtime)
#   7  module constant -> E4070
#   8  type mismatch with the outer binding -> E4031 at the init
#   9  nested loops: both bare inits assign their outer binding (runtime)
#  10  an inner bare init assigns the enclosing loop's `let` variable
#  11  guard: `lp(let i=..)` declares a fresh i; the outer i is untouched
#  12  guard: a `lp(let k=..)` variable is not in scope after the loop
#  13  guard: the one-clause `lp(cond)` form is unchanged
#  14  lint: a `mut.` binding assigned only by a bare init is not
#      "mutable-never-mutated"
#  15  module-level immutable `let` -> E4070 at the init, no fix, --check
#      fails (it was assigned silently)
#  16  import alias -> E4070 at the init, no fix, --check fails (it passed
#      --check and then failed in codegen with E9003)
#
# Negative control: TKC built from 685aa852 (main) -- 14 of 21 checks are
# red, at least one in each of cases 1-10 and 14 (a wrong value after the
# loop, no E4070 / E4031, E3011 at the condition instead of the init, a
# false lint warning); 11-13 and the "compiles clean" checks of 1, 5, 6
# and 9 pass on both.
#
# Story: 127.289

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
# "CODE@line:col" of the FIRST error
first_at() { "${TKC}" --check "$1" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    l = l.strip()
    if not l.startswith("{"): continue
    d = json.loads(l)
    if d.get("severity") != "error": continue
    print("%s@%d:%d" % (d["error_code"], d["pos"]["line"], d["pos"]["col"])); break'; }
# 1 when the FIRST error carries a fix field, else 0
first_fix() { "${TKC}" --check "$1" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    l = l.strip()
    if not l.startswith("{"): continue
    d = json.loads(l)
    if d.get("severity") != "error": continue
    print(1 if "fix" in d else 0); break'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
# "fails" when --check exits non-zero, else "passes"
check_rc() { if "${TKC}" --check "$1" >/dev/null 2>&1; then echo passes; else echo fails; fi; }

# 1. outer mut: the bare init assigns it, and after the loop it holds 5
cat > "${WORK}/p1.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let i=mut.7;
  let s=mut.0;
  lp(i=0;i<5;i=i+1){ s=s+i };
  io.println("\(i) \(s)");
  <0
};
EOF
expect "1-outer-mut-clean"         "$(errs "${WORK}/p1.tk")" ""
expect "1-outer-mut-final-counter" "$(runs "${WORK}/p1.tk")" "5 10 "

# 2. outer immutable let: E4070 at the init (line 4 col 6), no fix there
cat > "${WORK}/p2.tk" <<'EOF'
m=t;
f=main():i64{
  let i=7;
  lp(i=0;i<5;i=i+1){ };
  <i
};
EOF
expect "2-immutable-let-E4070-at-init" "$(first_at "${WORK}/p2.tk")" "E4070@4:6"
expect "2-immutable-let-init-no-fix"   "$(first_fix "${WORK}/p2.tk")" "0"

# 3. parameter: E4070 at the init, no fix
cat > "${WORK}/p3.tk" <<'EOF'
m=t;
f=go(i:i64):i64{
  lp(i=0;i<5;i=i+1){ };
  <i
};
f=main():i64{ <go(3) };
EOF
expect "3-param-E4070-at-init" "$(first_at "${WORK}/p3.tk")" "E4070@3:6"
expect "3-param-init-no-fix"   "$(first_fix "${WORK}/p3.tk")" "0"

# 4. match-arm binding: E4070 at the init
cat > "${WORK}/p4.tk" <<'EOF'
m=t;
t=$e{k:i64};
f=pick(x:i64):i64!$e{ <x };
f=main():i64{
  let r=mt pick(3){$ok:v if(v>0){lp(v=0;v<9;v=v+1){};v}el{v};$err:e 0};
  <r
};
EOF
expect "4-arm-binding-E4070" "$(first_at "${WORK}/p4.tk")" "E4070@5:37"

# 5. no binding named i in force: E3011 at the init, and only there
cat > "${WORK}/p5.tk" <<'EOF'
m=t;
f=main():i64{
  let s=mut.0;
  lp(i=0;i<5;i=i+1){ s=s+i };
  <s
};
EOF
expect "5-no-binding-E3011-at-init" "$(first_at "${WORK}/p5.tk")" "E3011@4:6"
expect "5-no-binding-reported-once" "$(errs "${WORK}/p5.tk")" "E3011 "

# 6. module mut global: the loop assigns it
cat > "${WORK}/p6.tk" <<'EOF'
m=t;
i=io:std.io;
let g=mut.10;
f=main():i64{
  lp(g=0;g<4;g=g+1){ };
  io.println("\(g)");
  <0
};
EOF
expect "6-global-clean"    "$(errs "${WORK}/p6.tk")" ""
expect "6-global-assigned" "$(runs "${WORK}/p6.tk")" "4 "

# 7. module constant: not assignable
cat > "${WORK}/p7.tk" <<'EOF'
m=t;
lim=3:i64;
f=main():i64{
  lp(lim=0;lim<4;lim=lim+1){ };
  <0
};
EOF
expect "7-constant-E4070" "$(first_at "${WORK}/p7.tk")" "E4070@4:6"

# 8. the init's value must have the binding's type
cat > "${WORK}/p8.tk" <<'EOF'
m=t;
f=main():i64{
  let s=mut."x";
  let j=mut.0;
  lp(s=0;j<4;j=j+1){ };
  <0
};
EOF
expect "8-type-mismatch-E4031" "$(first_at "${WORK}/p8.tk")" "E4031@5:6"

# 9. nested loops: each bare init assigns its outer binding
cat > "${WORK}/p9.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let i=mut.0;
  let j=mut.0;
  let n=mut.0;
  lp(i=0;i<2;i=i+1){
    lp(j=0;j<3;j=j+1){ n=n+1 }
  };
  io.println("\(i) \(j) \(n)");
  <0
};
EOF
expect "9-nested-clean" "$(errs "${WORK}/p9.tk")" ""
expect "9-nested-runs"  "$(runs "${WORK}/p9.tk")" "2 3 6 "

# 10. an inner bare init assigns the enclosing loop's variable: the inner
#     loop leaves i at 7, the outer step makes it 8, the outer loop ends
#     after one pass (t = 5+6)
cat > "${WORK}/p10.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let t=mut.0;
  lp(let i=0;i<2;i=i+1){
    lp(i=5;i<7;i=i+1){ t=t+i }
  };
  io.println("\(t)");
  <0
};
EOF
expect "10-inner-assigns-outer-loop-var" "$(runs "${WORK}/p10.tk")" "11 "

# 11. guard: `let` in the init declares a fresh i; the outer one is untouched
cat > "${WORK}/p11.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let i=mut.9;
  let s=mut.0;
  lp(let i=0;i<3;i=i+1){ s=s+i };
  io.println("\(i) \(s)");
  <0
};
EOF
expect "11-let-init-fresh-variable" "$(runs "${WORK}/p11.tk")" "9 3 "

# 12. guard: a let-init variable is not in scope after the loop
cat > "${WORK}/p12.tk" <<'EOF'
m=t;
f=main():i64{
  let s=mut.0;
  lp(let k=0;k<3;k=k+1){ s=s+k };
  <k
};
EOF
expect "12-let-init-scoped-to-loop" "$(first_at "${WORK}/p12.tk")" "E3011@5:4"

# 13. guard: the one-clause form
cat > "${WORK}/p13.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let x=mut.0;
  lp(x<5){ x=x+1 };
  io.println("\(x)");
  <0
};
EOF
expect "13-while-form" "$(runs "${WORK}/p13.tk")" "5 "

# 14. lint: x is assigned by the bare init, so it is mutated
cat > "${WORK}/p14.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let x=mut.0;
  let y=mut.0;
  lp(x=5;y<3;y=y+1){ };
  io.println("\(x)");
  <0
};
EOF
lint14="$("${TKC}" --lint "${WORK}/p14.tk" 2>&1 | grep -c 'mutable-never-mutated' || true)"
expect "14-lint-bare-init-is-a-write" "${lint14}" "0"
expect "14-runs"                       "$(runs "${WORK}/p14.tk")" "5 "

# 15. module-level immutable let: E4070 at the init (line 5 col 6), no fix
cat > "${WORK}/p15.tk" <<'EOF'
m=t;
i=io:std.io;
let g=5;
f=main():i64{
  lp(g=0;g<3;g=g+1){ };
  io.println("\(g)");
  <0
};
EOF
expect "15-module-let-E4070-at-init" "$(first_at "${WORK}/p15.tk")" "E4070@5:6"
expect "15-module-let-init-no-fix"   "$(first_fix "${WORK}/p15.tk")" "0"
expect "15-module-let-check-fails"   "$(check_rc "${WORK}/p15.tk")" "fails"

# 16. import alias: E4070 at the init (line 5 col 6), no fix
cat > "${WORK}/p16.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let j=mut.0;
  lp(io=0;j<3;j=j+1){ };
  <0
};
EOF
expect "16-import-alias-E4070-at-init" "$(first_at "${WORK}/p16.tk")" "E4070@5:6"
expect "16-import-alias-init-no-fix"   "$(first_fix "${WORK}/p16.tk")" "0"
expect "16-import-alias-check-fails"   "$(check_rc "${WORK}/p16.tk")" "fails"

echo "C1820: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
