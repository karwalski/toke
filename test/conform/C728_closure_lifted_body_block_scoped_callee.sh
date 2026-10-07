#!/usr/bin/env bash
# C728_closure_lifted_body_block_scoped_callee.sh — inside a closure body, a
# call of a name the closure does not bind is NOT a call of some other
# closure-bound local of that name (story 127.282).
#
# THE DEFECT. Lifted closure bodies are emitted at module tail and look up
# closure-bound locals in one module-wide table by name.  The 127.196 block
# restore entries are visible only to the f= that made them, so after
#
#     if(c==1){ let h=fn(a:i64):i64{<a*10}; let g=fn(b:i64):i64{<h(b)+1}; .. };
#     let g=fn(b:i64):i64{<h(b)+2};          // h is the module f=h here
#
# the second closure's body still found the dead block-local closure `h`
# and called the module function's ADDRESS as a closure env box: a clean
# compile and a segfault (exit 139).  The same happened with no block at all
# when the same-named closure-bound local lived in ANOTHER function (cases
# 7, 13).
#
# THE FIX (src/llvm.c clos_callee_lookup): in a lifted body, a closure-bound
# callee must be one of the body's own locals (a capture, a parameter or its
# own `let`); otherwise the call resolves as the checker resolved it.
#
# Negative control: TKC built from 7ca0e318 (main) — cases 1-5, 7-11, 13-15
# fail with exit 139 (13 of 15 red); 6 and 12 are guards that pass on both.
#
# Story: 127.282

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
# compile + run: the program's output, then its exit status
runs() {
    rm -f "${WORK}/b"
    if ! "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1; then echo "compile-failed"; return; fi
    local out rc
    out="$("${WORK}/b" 2>/dev/null | tr '\n' ' ')"
    "${WORK}/b" >/dev/null 2>&1; rc=$?
    echo "${out}exit ${rc}"
}

# 1. the 127.282 repro: block-local closure h, later module f=h
cat > "${WORK}/p1.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<h(b)+1};
    io.println("in \(g(2))");
  };
  let g=fn(b:i64):i64{<h(b)+2};
  io.println("out \(g(2))");
  <0
};
EOF
expect "1-repro" "$(runs "${WORK}/p1.tk")" "in 21 out 104 exit 0"

# 2. nested blocks: h in the outer block, g in the inner one
cat > "${WORK}/p2.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    if(c==1){
      let g=fn(b:i64):i64{<h(b)+1};
      io.println("in \(g(2))");
    };
  };
  let g=fn(b:i64):i64{<h(b)+2};
  io.println("out \(g(2))");
  <0
};
EOF
expect "2-nested" "$(runs "${WORK}/p2.tk")" "in 21 out 104 exit 0"

# 3. an lp body binds h and a capturing g each iteration
cat > "${WORK}/p3.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  lp(let j=0;j<2;j=j+1){
    let h=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<h(b)+j};
    io.println("in \(g(2))");
  };
  let g=fn(b:i64):i64{<h(b)+2};
  io.println("out \(g(2))");
  <0
};
EOF
expect "3-loop" "$(runs "${WORK}/p3.tk")" "in 20 in 21 out 104 exit 0"

# 4. same-named closures in sibling blocks
cat > "${WORK}/p4.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<h(b)+1};
    io.println("a \(g(2))");
  };
  if(c==1){
    let h=fn(a:i64):i64{<a*3};
    let g=fn(b:i64):i64{<h(b)+5};
    io.println("b \(g(2))");
  };
  let g=fn(b:i64):i64{<h(b)+2};
  io.println("out \(g(2))");
  <0
};
EOF
expect "4-siblings" "$(runs "${WORK}/p4.tk")" "a 21 b 11 out 104 exit 0"

# 5. a closure capturing a closure that captures; module f=k and f=h after
cat > "${WORK}/p5.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    let k=fn(b:i64):i64{<h(b)+1};
    let g=fn(b:i64):i64{<k(b)+1};
    io.println("in \(g(2))");
  };
  let g=fn(b:i64):i64{<k(b)+h(b)};
  io.println("out \(g(2))");
  <0
};
EOF
expect "5-chain" "$(runs "${WORK}/p5.tk")" "in 22 out 304 exit 0"

# 6. guard: a block-local closure shadowing a module global
cat > "${WORK}/p6.tk" <<'EOF'
m=a;
i=io:std.io;
let q=mut.7;
f=main():i64{
  let c=1;
  if(c==1){
    let q=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<q(b)+1};
    io.println("in \(g(2))");
  };
  let g=fn(b:i64):i64{<q+b};
  io.println("out \(g(2))");
  <0
};
EOF
expect "6-global-guard" "$(runs "${WORK}/p6.tk")" "in 21 out 9 exit 0"

# 7. no block: the same-named closure-bound local is in another f=
cat > "${WORK}/p7.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=other():i64{
  let g=fn(b:i64):i64{<h(b)+2};
  <g(2)
};
f=main():i64{
  let h=fn(a:i64):i64{<a*10};
  let g=fn(b:i64):i64{<h(b)+1};
  io.println("in \(g(2))");
  io.println("out \(other())");
  <0
};
EOF
expect "7-other-fn" "$(runs "${WORK}/p7.tk")" "in 21 out 104 exit 0"

# 8. the block is inside the closure body itself
cat > "${WORK}/p8.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let g=fn(b:i64):i64{
    if(b>0){
      let h=fn(a:i64):i64{<a*10};
      io.println("in \(h(b))");
    };
    <h(b)+2
  };
  io.println("out \(g(2))");
  <0
};
EOF
expect "8-block-in-body" "$(runs "${WORK}/p8.tk")" "in 20 out 104 exit 0"

# 9. closures nested in a closure, then a module-fn caller outside
cat > "${WORK}/p9.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let o=fn(x:i64):i64{
    let h=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<h(b)+1};
    <g(x)
  };
  io.println("in \(o(2))");
  let g=fn(b:i64):i64{<h(b)+2};
  io.println("out \(g(2))");
  <0
};
EOF
expect "9-nested-lifted" "$(runs "${WORK}/p9.tk")" "in 21 out 104 exit 0"

# 10. a differently named caller, and a direct call after the block
cat > "${WORK}/p10.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<h(b)+1};
    io.println("in \(g(2))");
  };
  let w=fn(b:i64):i64{<h(b)+2};
  io.println("out \(w(2)) \(h(3))");
  <0
};
EOF
expect "10-direct" "$(runs "${WORK}/p10.tk")" "in 21 out 104 103 exit 0"

# 11. both arms of an if/el bind h
cat > "${WORK}/p11.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let c=1;
  let x=mut.0;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<h(b)+1};
    x=g(2);
  }el{
    let h=fn(a:i64):i64{<a*7};
    let g=fn(b:i64):i64{<h(b)+1};
    x=g(3);
  };
  let g=fn(b:i64):i64{<h(b)+x};
  io.println("out \(g(2))");
  <0
};
EOF
expect "11-if-el" "$(runs "${WORK}/p11.tk")" "out 123 exit 0"

# 12. guard: an outer closure h re-bound after the block
cat > "${WORK}/p12.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    let g=fn(b:i64):i64{<h(b)+1};
    io.println("in \(g(2))");
  };
  let h=fn(a:i64):i64{<a*4};
  let g=fn(b:i64):i64{<h(b)+2};
  io.println("out \(g(2))");
  <0
};
EOF
expect "12-rebind-guard" "$(runs "${WORK}/p12.tk")" "in 21 out 10 exit 0"

# 13. the module-fn caller is a closure in another f=
cat > "${WORK}/p13.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=ap(x:i64):i64{
  let g=fn(b:i64):i64{<h(b)+x};
  <g(1)
};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    io.println("in \(h(2))");
  };
  io.println("out \(ap(3)) \(h(1))");
  <0
};
EOF
expect "13-other-fn-closure" "$(runs "${WORK}/p13.tk")" "in 20 out 104 101 exit 0"

# 14. the block-local h has another signature (str)
cat > "${WORK}/p14.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=k(a:i64):i64{<a+200};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:str):str{<a};
    let g=fn(b:str):str{<h(b)};
    io.println("in \(g("x"))");
  };
  let g=fn(b:i64):i64{<h(b)+2};
  io.println("out \(g(2))");
  <0
};
EOF
expect "14-other-sig" "$(runs "${WORK}/p14.tk")" "in x out 104 exit 0"

# 15. two block-local closures, one re-bound outside
cat > "${WORK}/p15.tk" <<'EOF'
m=a;
i=io:std.io;
f=h(a:i64):i64{<a+100};
f=main():i64{
  let c=1;
  if(c==1){
    let h=fn(a:i64):i64{<a*10};
    let t=fn(b:i64):i64{<b+1};
    let g=fn(b:i64):i64{<h(t(b))};
    io.println("in \(g(2))");
  };
  let t=fn(b:i64):i64{<b+5};
  let g=fn(b:i64):i64{<h(t(b))};
  io.println("out \(g(2))");
  <0
};
EOF
expect "15-partial-rebind" "$(runs "${WORK}/p15.tk")" "in 30 out 107 exit 0"

echo "C728: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
