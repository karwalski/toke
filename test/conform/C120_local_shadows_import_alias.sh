#!/usr/bin/env bash
# C120_local_shadows_import_alias.sh — a parameter or local named like an
# `I=` import alias is a VALUE: `s.slice(0;1)` on it is a receiver call, in
# the checker and in codegen alike (story 127.214).
#
# THE DEFECT. With `i=s:std.str;` in force,
#
#     f=first(s:str):str{<s.slice(0;1)};
#
# was checked as the module call std.str.slice(0;1) — a false E4026 "takes 3
# arguments, the call passes 2" — and the same for `let s="hey";
# s.slice(0;1)`.  Behind the checker, codegen made the same mistake: it
# lowered the call as `tk_str_slice_w(0, 1)` (two arguments into a
# three-argument C function), so removing the E4026 alone turns a false
# rejection into a wrong answer.  Both stages now ask whether the name is a
# local in force first (types.c alias_shadowed, llvm.c name_is_local) — the
# 75.1.7 shadowing decision, and the 127.169 rule for functions.
#
# WHAT IS ASSERTED. Each program must --check clean AND build and print the
# receiver-method answer:
#   1. a parameter named like the alias                        (paramshadow)
#   2. a `let` named like the alias, after the let            (letshadow)
#   3. a closure parameter named like the alias
#   4. the alias used as a module BEFORE the let, the local after it
#   5. `let s=s.trim(..)` — the initialiser is still the module call
#   6. positive guard: with no local of that name, the module call is still
#      checked — `s.slice("x";0)` is E4026
#   7. the converse: `f(s:str){<s.upper(s)}` meant the module and is now a
#      receiver call with one argument too many — E4026, and the message
#      names the shadowing (25 corpus records and toke-eval 0076/0162/0233/
#      0281 are this shape)
#
# Negative control: TKC built from main at 8f4a843 — cases 1-5 are E4026 and
# case 7 is clean: 12 of 14 red.  A mutant with the types.c half only
# (llvm.c as on main) checks clean and then prints nothing for cases 1-4:
# the llvm.c half is load-bearing.
#
# Story: 127.214

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

# 1. a parameter
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=s:std.str;i=io:std.io;
f=first(s:str):str{<mt s.slice(0;1){$ok:v v;$err:e ""}};
f=up(s:str):str{<s.upper()};
f=main():i64{io.println(first("hey"));io.println(up("abc"));<0};
EOF
expect "1-param-checks" "$(errs "${WORK}/p1.tk")" ""
expect "1-param-runs"   "$(runs "${WORK}/p1.tk")" "h ABC "

# 2. a let, after it is bound
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=s:std.str;i=io:std.io;
f=main():i64{let s="hello";io.println(mt s.slice(1;3){$ok:v v;$err:e ""});io.println(s.trim());<0};
EOF
expect "2-let-checks" "$(errs "${WORK}/p2.tk")" ""
expect "2-let-runs"   "$(runs "${WORK}/p2.tk")" "el hello "

# 3. a closure parameter
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=s:std.str;i=io:std.io;
f=main():i64{let g=fn(s:str):str{<s.upper()};io.println(g("zz"));<0};
EOF
expect "3-closure-param-checks" "$(errs "${WORK}/p3.tk")" ""
expect "3-closure-param-runs"   "$(runs "${WORK}/p3.tk")" "ZZ "

# 4. the module before the let, the local (a str array) after it
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=s:std.str;i=io:std.io;
f=main():i64{
  io.println(mt s.slice("before";0;2){$ok:v v;$err:e ""});
  let s=@("a";"b";"c");
  io.println("\(s.len)");
  io.println(s.join(","));
  <0
};
EOF
expect "4-before-and-after-checks" "$(errs "${WORK}/p4.tk")" ""
expect "4-before-and-after-runs"   "$(runs "${WORK}/p4.tk")" "be 3 a,b,c "

# 5. the initialiser of the shadowing let is still the module call
cat > "${WORK}/p5.tk" <<'EOF'
m=t;i=s:std.str;i=io:std.io;
f=main():i64{let s=s.trim("  x  ");io.println(s.upper());<0};
EOF
expect "5-init-is-module-checks" "$(errs "${WORK}/p5.tk")" ""
expect "5-init-is-module-runs"   "$(runs "${WORK}/p5.tk")" "X "

# 6. positive guard: unshadowed, the module call is still arity-checked
cat > "${WORK}/p6.tk" <<'EOF'
m=t;i=s:std.str;i=io:std.io;
f=main():i64{io.println(mt s.slice("x";0){$ok:v v;$err:e ""});<0};
EOF
expect "6-module-call-still-checked" "$(errs "${WORK}/p6.tk")" "E4026 "

# 7. the other side of the rule: a module call written through a shadowing
#    parameter (`s.upper(s)`) is now a receiver call with a surplus argument,
#    and the message says why — that is the thing to repair
cat > "${WORK}/p7.tk" <<'EOF'
m=t;i=s:std.str;i=io:std.io;
f=up(s:str):str{<s.upper(s)};
f=main():i64{io.println(up("a"));<0};
EOF
expect "7-shadowed-module-call-is-E4026" "$(errs "${WORK}/p7.tk")" "E4026 "
expect "7-message-names-the-shadowing" \
  "$("${TKC}" --check "${WORK}/p7.tk" 2>&1 | grep -c "shadows the import alias 's' of std.str")" "1"

echo "C120: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
