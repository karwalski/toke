#!/usr/bin/env bash
# C065_local_shadows_module_fn.sh — a local binding shadows a module function
# of the same name in the checker exactly where it does in codegen
# (story 127.169).
#
# THE DEFECT. The checker looked a name up at module scope first and only
# fell back to the function's local bindings when module scope had nothing,
# so a `let` named like a module function was typed as the function:
#
#     f=base(p:$str):$str{<p};
#     f=c(p:$str):$str{let base="x";<base};    E4031 "expected str, got func"
#
# (stdlib/path.tk:71/97, 127.126), and `let sum=fn..; sum(a)` was checked
# against the module `sum` while codegen called the closure.
#
# WHAT IS ASSERTED. Codegen's resolution, measured at d25ba32, is the spec
# the checker now follows (local_in_force, types.c):
#   1. a use after the `let`                  -> the local
#   2. a use BEFORE the `let`                 -> the module function
#   3. after an `if` whose block holds the let -> the module function
#      (127.196: block scoping -- the block's `let` ended with the block;
#      before it this asserted the flat-table leak, "out 3")
#   4. a let inside a closure, use outside     -> the module function
# Each program must --check clean AND run to the output that proves which
# binding codegen used; 5 is a closure call whose argument only the
# CLOSURE accepts, so checking it against the module `sum` would be E4031.
#
# Negative control: TKC built from d25ba32 — cases 1, 2 and 5 are E4031, so
# they neither check nor build (6 of 11 red).
#
# Story: 127.169 (case 3 revised by 127.196)

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

# 1. the 127.126 shape: a bare use of the local after it is bound
cat > "${WORK}/p1.tk" <<'EOF'
m=t;
i=io:std.io;
f=base(p:$str):$str{<"FN"};
f=c(p:$str):$str{let base="LOCAL";<base};
f=main():i64{
  io.println(c("q"));
  <0
};
EOF
expect "1-local-after-let-checks" "$(errs "${WORK}/p1.tk")" ""
expect "1-local-after-let-runs"   "$(runs "${WORK}/p1.tk")" "LOCAL "

# 2 + 1 in one function: before the let the module function, after it the local
cat > "${WORK}/p2.tk" <<'EOF'
m=t;
i=io:std.io;
f=base(p:$str):$str{<"FN"};
f=main():i64{
  io.println(base("q"));
  let base="LOCAL";
  io.println(base);
  <0
};
EOF
expect "2-before-and-after-checks" "$(errs "${WORK}/p2.tk")" ""
expect "2-before-and-after-runs"   "$(runs "${WORK}/p2.tk")" "FN LOCAL "

# 3. a let inside an if block is out of scope after the block: the call there
#    is the module function again (127.196, block scoping)
cat > "${WORK}/p3.tk" <<'EOF'
m=t;
i=io:std.io;
f=sum(a:@(i64)):i64{<99};
f=main():i64{
  let a=@(1;2;3);
  if(a.len>0){
    let sum=fn(x:@(i64)):i64{<3};
    io.println("in \(sum(a))");
  };
  io.println("out \(sum(a))");
  <0
};
EOF
expect "3-block-let-checks" "$(errs "${WORK}/p3.tk")" ""
expect "3-block-let-runs"   "$(runs "${WORK}/p3.tk")" "in 3 out 99 "

# 4. a let inside a closure body does not reach the enclosing function
cat > "${WORK}/p4.tk" <<'EOF'
m=t;
i=io:std.io;
f=sum(a:@(i64)):i64{<99};
f=main():i64{
  let a=@(1;2;3);
  let g=fn(b:@(i64)):i64{
    let sum=fn(x:@(i64)):i64{<3};
    <sum(b)
  };
  io.println("g \(g(a)) after \(sum(a))");
  <0
};
EOF
expect "4-closure-let-checks" "$(errs "${WORK}/p4.tk")" ""
expect "4-closure-let-runs"   "$(runs "${WORK}/p4.tk")" "g 3 after 99 "

# 5. the call is checked against the closure, not the module signature
cat > "${WORK}/p5.tk" <<'EOF'
m=t;
i=io:std.io;
f=sum(a:$str):$str{<a};
f=main():i64{
  let sum=fn(a:@(i64)):i64{<3};
  let a=@(1;2;3);
  let n=sum(a);
  io.println("\(n+1)");
  <0
};
EOF
expect "5-closure-call-checks" "$(errs "${WORK}/p5.tk")" ""
expect "5-closure-call-runs"   "$(runs "${WORK}/p5.tk")" "4 "

# 6. positive guard: with no local of that name the module function is still
#    checked — a wrong argument is still E4031
cat > "${WORK}/p6.tk" <<'EOF'
m=t;
f=base(p:$str):$str{<p};
f=main():i64{
  let r=base(1);
  <0
};
EOF
expect "6-module-fn-still-checked" "$(errs "${WORK}/p6.tk")" "E4031 "

echo "C065: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
