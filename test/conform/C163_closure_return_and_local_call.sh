#!/usr/bin/env bash
# C163_closure_return_and_local_call.sh — a `<` in a closure returns from the
# closure, and a call through a closure-bound local has the closure's result
# type (story 127.224 item 3; 127.223 items 1 and 2).
#
# THE DEFECTS, all on main at a705c93:
#   * types.c had no NODE_CLOSURE case, so a closure body was walked under the
#     ENCLOSING function's return type: `fn(s:str):str{<"x"}` inside
#     `main():i64` was a false E4031 "expected 'i64', got 'str'", and after a
#     nested closure the outer closure's `<` was judged the same way.
#   * a call through a closure-bound local was TY_UNKNOWN; the emitter then
#     read a same-named MODULE function's signature.  With `f=sum(a:$str):$str`
#     declared, `"\(sum(@(1;2)))"` on a local `let sum=fn(..):i64` rendered the
#     i64 result as a string pointer — clean --check, exit 139.  And a closure
#     declared `:str` interpolated as an address.
#
# Negative control: TKC built from main at a705c93 — cases 1-8 red (case 9
# passes on main: it leaks instead of freeing, which is not a use-after-free) (case 3:
# main checked the closure's `<3` against main's i64 and accepted it).
#
# Story: 127.224 (items 3 and 8), 127.223, 124.0c item 3

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
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1; "${WORK}/b" 2>/dev/null | tr '\n' ' '; echo "rc=${PIPESTATUS[0]}"; }

# 1. a str closure inside an i64 function
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let g=fn(s:str):str{<"x"};
  io.println(g("a"));
  <0
};
EOF
expect "1-closure-return-own-type" "$(codes "${WORK}/p1.tk")" ""

# 2. the outer closure's return after a nested closure
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let g=fn(b:i64):$str{let h=fn(x:i64):i64{<3};<"n"};
  io.println(g(1));
  <0
};
EOF
expect "2-nested-closure-outer-return" "$(codes "${WORK}/p2.tk")" ""

# 3. positive guard: a closure's `<` is still checked, against the closure
cat > "${WORK}/p3.tk" <<'EOF'
m=t;
f=main():i64{
  let g=fn(s:str):str{<3};
  <0
};
EOF
expect "3-closure-return-mismatch" "$(codes "${WORK}/p3.tk")" "E4031 "

# 4. a closure-local `sum` shadowing a module `sum`, interpolated in a closure
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;
f=sum(a:$str):$str{<a};
f=main():i64{
  let g=fn(b:i64):i64{let sum=fn(x:@(i64)):i64{<3};io.println("n=\(sum(@(1;2)))");<sum(@(b))};
  io.println("\(g(4))");
  <0
};
EOF
expect "4-closure-local-shadows-module-fn" "$(runs "${WORK}/p4.tk")" "n=3 3 rc=0"

# 5. the same in a plain function body
cat > "${WORK}/p5.tk" <<'EOF'
m=t;i=io:std.io;
f=sum(a:$str):$str{<a};
f=main():i64{
  let sum=fn(x:@(i64)):i64{<3};
  io.println("n=\(sum(@(1;2)))");
  <0
};
EOF
expect "5-local-closure-shadows-module-fn" "$(runs "${WORK}/p5.tk")" "n=3 rc=0"

# 6. a str-returning closure interpolates as a string
cat > "${WORK}/p6.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let h=fn(x:i64):str{<"a"};
  let k=h(2);
  io.println("[\(h(1))][\(k)]");
  <0
};
EOF
expect "6-str-closure-interp" "$(runs "${WORK}/p6.tk")" "[a][a] rc=0"

# 7. (127.224 item 8) more than 64 closure-bound locals in one module: the
#    table was a fixed 64, the 65th binding was dropped, and its call lowered
#    as its own argument — g4 below returned 56 instead of 125, exit 0.
python3 - "${WORK}/p7.tk" <<'PY'
import sys
L = ["m=t;", "i=io:std.io;"]
for k in range(5):
    L.append("f=g%d(x:i64):i64{" % k)
    for i in range(14):
        L.append("  let c%d=fn(y:i64):i64{<y+%d};" % (i, k * 14 + i))
    L.append("  <c0(x)+c13(x)")
    L.append("};")
L += ["f=main():i64{", '  io.println("\\(g0(0)) \\(g4(0))");', "  <0", "};"]
open(sys.argv[1], "w").write("\n".join(L) + "\n")
PY
expect "7-closure-table-past-64" "$(runs "${WORK}/p7.tk")" "13 125 rc=0"

# 8-9. (124.0c item 3, memory-model §2.5) a closure bound to a local releases
#    its environment.  Before, only a closure applied on the spot did; a bound
#    one leaked its malloc'd box once per execution of the `let`.  Now a
#    binding proven to hold the ONLY reference (every other occurrence of the
#    name is a call) frees the previous box when the `let` re-runs and the
#    live one at every return.
#    8: 3,000,000 loop iterations at -O0 (at -O2 LLVM removes the box
#       entirely): main peaks at ~49 MB (16 B leaked per iteration), now
#       ~1.5 MB.  The bound is 20 MB.
#    9: use-after-free check under a scribbling allocator — GuardMalloc on
#       macOS, MALLOC_PERTURB_ on glibc — over returns, an error return, a
#       loop re-bind, and two closures that ESCAPE (copied to another local,
#       captured by another closure) and so must not be freed.
#    Negative control for 9: a compiler that also frees the box right after
#    the `let` (a use-after-free by construction) exits 139 under
#    GuardMalloc; this compiler prints the expected line.
cat > "${WORK}/p8.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let s=mut.0;
  lp(let i=0;i<3000000;i=i+1){
    let h=fn(x:i64):i64{<x+i};
    s=s+h(1);
  };
  io.println("\(s)");
  <0
};
EOF
maxrss_kb() {   # BIN -> peak RSS in KiB, or empty
    if [ "$(uname)" = Darwin ]; then
        /usr/bin/time -l "$1" 2>&1 >/dev/null | awk '/maximum resident set size/{print int($1/1024)}'
    else
        /usr/bin/time -v "$1" 2>&1 >/dev/null | awk -F: '/Maximum resident set size/{gsub(/ /,"",$2); print $2}'
    fi
}
if "${TKC}" -O0 "${WORK}/p8.tk" -o "${WORK}/b8" >/dev/null 2>&1; then
    kb="$(maxrss_kb "${WORK}/b8")"
    if [ -n "${kb}" ] && [ "${kb}" -lt 20480 ]; then echo "PASS 8-bound-closure-env-released (${kb} KiB peak)"; PASS=$((PASS + 1))
    else echo "FAIL 8-bound-closure-env-released"; echo "        peak RSS ${kb:-unmeasured} KiB, bound 20480"; FAIL=$((FAIL + 1)); fi
else echo "FAIL 8-bound-closure-env-released (build failed)"; FAIL=$((FAIL + 1)); fi

cat > "${WORK}/p9.tk" <<'EOF'
m=t;i=io:std.io;
t=$e{$bad:i64};
f=mk(n:i64):i64{
  let g=fn(x:i64):i64{<x+n};
  let h=g;
  let k=fn(x:i64):i64{<g(x)*2};
  <h(1)+k(1)
};
f=chk(n:i64):i64!$e{
  let g=fn(x:i64):i64{<x*n};
  if(n>5){<$e{$bad:g(1)}};
  <g(3)
};
f=main():i64{
  let a=mk(4);
  let b=mt chk(2) {$ok:v v; $err:e 0-1};
  let c=mt chk(9) {$ok:v v; $err:e 0-2};
  let s=mut.0;
  lp(let i=0;i<50;i=i+1){
    let h=fn(x:i64):i64{<x+i};
    if(i%2==0){ s=s+h(1) };
  };
  io.println("\(a) \(b) \(c) \(s)");
  <0
};
EOF
if "${TKC}" -O0 "${WORK}/p9.tk" -o "${WORK}/b9" >/dev/null 2>&1; then
    if [ "$(uname)" = Darwin ] && [ -e /usr/lib/libgmalloc.dylib ]; then
        got="$(DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib "${WORK}/b9" 2>/dev/null | tr '\n' ' '; echo "rc=${PIPESTATUS[0]}")"
    else
        got="$(MALLOC_PERTURB_=85 MALLOC_CHECK_=3 "${WORK}/b9" 2>/dev/null | tr '\n' ' '; echo "rc=${PIPESTATUS[0]}")"
    fi
    expect "9-released-envs-no-use-after-free" "${got}" "11 6 -2 625 rc=0"
else echo "FAIL 9-released-envs-no-use-after-free (build failed)"; FAIL=$((FAIL + 1)); fi

echo "C163: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
