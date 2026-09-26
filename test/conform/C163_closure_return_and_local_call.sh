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
# Negative control: TKC built from main at a705c93 — all seven red (case 3:
# main checked the closure's `<3` against main's i64 and accepted it).
#
# Story: 127.224 (items 3 and 8), 127.223

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

echo "C163: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
