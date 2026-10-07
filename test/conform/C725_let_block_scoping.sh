#!/usr/bin/env bash
# C725_let_block_scoping.sh — a `let` (or `lp` variable) is scoped to its
# block, in the checker AND the emitter (story 127.196).
#
# THE DEFECT. The emitter kept one flat locals table per function and the
# checker mirrored it (127.169), so a binding made inside a block outlived
# the block wherever the name also meant something outside it:
#
#     f=sum(a:i64):i64{<a+100};
#     if(c==1){let sum=fn(a:i64):i64{<a};};  sum(1)      printed 1, not 101
#
# and likewise for a module `mut` global read after the block, a `let x=&f`
# function reference, and an `lp(let i=..)` variable that shadowed an outer
# `i` (its last value leaked out of the loop).  The checker's mutability test
# was flat too: an assignment after the block was E4070 when the block held
# an immutable `let` of a global's name, and an outer immutable `let` was
# silently assignable when some block held a `mut` of its name.
#
# THE RULE (docs/spec/toke-spec-v0.4.md §I, semantics.md §4): a binding is in
# scope from its statement to the end of its innermost block; afterwards the
# name means what it meant before the block.  Shadowing is allowed (v0.3
# §11.8.1) and ends with the shadowing block.
#
# Negative control: TKC built from 380de510 (main) — cases 1-7 fail (9 of 16
# red: wrong output, a false E4070, an accepted assignment to an immutable
# binding, a false E4031); 8-11 are guards that pass on both.
#
# Story: 127.196

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
# error codes only (W3013, the enclosing-shadow warning, is expected here)
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

# 1. a closure-bound `let` in a block shadows a module function only there
cat > "${WORK}/p1.tk" <<'EOF'
m=t;
i=io:std.io;
f=sum(a:i64):i64{<a+100};
f=main():i64{
  let c=1;
  if(c==1){ let sum=fn(a:i64):i64{<a}; io.println("in \(sum(1))"); };
  io.println("out \(sum(1))");
  <0
};
EOF
expect "1-closure-let-checks" "$(errs "${WORK}/p1.tk")" ""
expect "1-closure-let-runs"   "$(runs "${WORK}/p1.tk")" "in 1 out 101 "

# 2. a block's `let` of a module global's name: the global after the block
cat > "${WORK}/p2.tk" <<'EOF'
m=t;
i=io:std.io;
let g=mut.10;
f=main():i64{
  let c=1;
  if(c==1){ let g=5; io.println("in \(g)"); };
  io.println("out \(g)");
  <0
};
EOF
expect "2-global-read-runs" "$(runs "${WORK}/p2.tk")" "in 5 out 10 "

# 3. ...and an assignment after the block assigns the (mutable) global
cat > "${WORK}/p3.tk" <<'EOF'
m=t;
i=io:std.io;
let g=mut.10;
f=main():i64{
  let c=1;
  if(c==1){ let g=5; io.println("in \(g)"); };
  g=g+1;
  io.println("out \(g)");
  <0
};
EOF
expect "3-global-assign-checks" "$(errs "${WORK}/p3.tk")" ""
expect "3-global-assign-runs"   "$(runs "${WORK}/p3.tk")" "in 5 out 11 "

# 4. an `lp` variable that shadows an outer `let` ends with the loop
cat > "${WORK}/p4.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let i=5;
  lp(let i=0;i<2;i=i+1){ io.println("in \(i)"); };
  io.println("out \(i)");
  <0
};
EOF
expect "4-loop-var-runs" "$(runs "${WORK}/p4.tk")" "in 0 in 1 out 5 "

# 5. a function-reference `let` in a block
cat > "${WORK}/p5.tk" <<'EOF'
m=t;
i=io:std.io;
f=sum(a:i64):i64{<a+100};
f=dbl(a:i64):i64{<a*2};
f=main():i64{
  let c=1;
  if(c==1){ let sum=&dbl; io.println("in \(sum(1))"); };
  io.println("out \(sum(1))");
  <0
};
EOF
expect "5-funcref-let-runs" "$(runs "${WORK}/p5.tk")" "in 2 out 101 "

# 6. a `mut` in a closed block does not make the outer immutable `let`
#    assignable: E4070 (main accepted it and stored 7 into the immutable x)
cat > "${WORK}/p6.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let x=1;
  let c=1;
  if(c==1){ let x=mut.5; x=6; io.println("in \(x)"); };
  x=7;
  io.println("out \(x)");
  <0
};
EOF
expect "6-outer-immutable-E4070" "$(errs "${WORK}/p6.tk")" "E4070 "

# 7. the checker types a name after a loop by the outer binding, not by the
#    loop variable (main: false E4031 "expected str, got i64")
cat > "${WORK}/p7.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let a=@("x";"y");
  let s=a.get(0);
  lp(let s=0;s<2;s=s+1){ io.println("\(s)"); };
  io.println(s);
  <0
};
EOF
expect "7-after-loop-checks" "$(errs "${WORK}/p7.tk")" ""
expect "7-after-loop-runs"   "$(runs "${WORK}/p7.tk")" "0 1 x "

# Guards (pass on main too).
# 8. a block-local name used after its block, with no outer meaning: E3011
cat > "${WORK}/p8.tk" <<'EOF'
m=t;
f=main():i64{
  let c=1;
  if(c==1){ let x=5; };
  <x
};
EOF
expect "8-use-after-block-E3011" "$(errs "${WORK}/p8.tk")" "E3011 "

# 9. a shadowing `let` ends with its block; the outer binding is back
cat > "${WORK}/p9.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let x=1;
  let h=fn(a:i64):i64{<a+1};
  if(x==1){ let x=2; let h=fn(a:i64):i64{<a*10}; io.println("in \(x) \(h(2))"); };
  io.println("out \(x) \(h(2))");
  <0
};
EOF
expect "9-shadow-ends-runs" "$(runs "${WORK}/p9.tk")" "in 2 20 out 1 3 "

# 10. same-named lets in sibling blocks, different types
cat > "${WORK}/p10.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let c=1;
  if(c==1){ let y="a"; io.println(y); };
  if(c==1){ let y=7; io.println("\(y+1)"); };
  <0
};
EOF
expect "10-siblings-checks" "$(errs "${WORK}/p10.tk")" ""
expect "10-siblings-runs"   "$(runs "${WORK}/p10.tk")" "a 8 "

# 11. same-scope rebinding (v0.3 §11.8.1): the nearest `let` is in force
cat > "${WORK}/p11.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let x=5;
  let x=mut.10;
  x=x+1;
  io.println("\(x)");
  <0
};
EOF
expect "11-same-scope-rebind-checks" "$(errs "${WORK}/p11.tk")" ""
expect "11-same-scope-rebind-runs"   "$(runs "${WORK}/p11.tk")" "11 "

echo "C725: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
