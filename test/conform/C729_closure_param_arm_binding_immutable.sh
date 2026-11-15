#!/usr/bin/env bash
# C729_closure_param_arm_binding_immutable.sh — a closure parameter and an
# `mt` arm binding are immutable: assigning one is E4070 (story 127.281).
#
# THE DEFECT. The E4070 check (types.c NODE_ASSIGN_STMT) treated an
# immutable `let` and the enclosing FUNCTION's own parameters as
# immutable, and nothing else, so both of these compiled and ran:
#
#     let g=fn(a:i64):i64{ a=a+1; <a };
#     mt pick(3){$ok:v if(v>0){v=v+1;v}el{v};$err:e 0}
#
# THE RULE. docs/spec/semantics.md §2.9: the binding an assignment targets
# "must be mutable: declared with `let x=mut.`, or the loop variable
# introduced by an `lp(let ..)` init" (wording amended by 127.283 and
# 127.289); §4.2: a parameter is not mutable.  A closure parameter is a
# parameter, and an arm binding is neither.  No `fix`: neither has a `let`
# to rewrite (AGENTS §6).
#
# Negative control: TKC built from 7ca0e318 (main) — cases 1-4 are red (no
# E4070; they compile); 5-9 are guards that pass on both.
#
# Story: 127.281

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
# error codes only (W3013, the enclosing-shadow warning, is not asserted)
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
hasfix() { local o; o="$("${TKC}" --check "$1" 2>&1 | grep '"E4070"')"; case "$o" in *'"fix"'*) echo yes ;; *) echo no ;; esac; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

# 1. a closure parameter assigned in the closure body
cat > "${WORK}/p1.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let g=fn(a:i64):i64{ a=a+1; <a };
  io.println("\(g(4))");
  <0
};
EOF
expect "1-closure-param-E4070" "$(errs "${WORK}/p1.tk")" "E4070 "
expect "1-closure-param-no-fix" "$(hasfix "${WORK}/p1.tk")" "no"

# 2. an error-union `mt` arm binding assigned in its arm
cat > "${WORK}/p2.tk" <<'EOF'
m=t;
i=io:std.io;
t=$e{$bad:i64};
f=pick(x:i64):i64!$e{ if(x>0){<x}; <$e{$bad:x} };
f=main():i64{
  let k=mt pick(3){$ok:v if(v>0){v=v+1;v}el{v};$err:e 0};
  io.println("\(k)");
  <0
};
EOF
expect "2-ok-arm-binding-E4070" "$(errs "${WORK}/p2.tk")" "E4070 "
expect "2-ok-arm-binding-no-fix" "$(hasfix "${WORK}/p2.tk")" "no"

# 3. a sum-type `mt` arm binding assigned in its arm
cat > "${WORK}/p3.tk" <<'EOF'
m=t;
i=io:std.io;
t=$sh{$nm:$str;$n:i64};
f=main():i64{
  let x=$sh{$n:4};
  let r=mt x{$nm:s 0;$n:k if(k>0){k=k*2;k}el{0}};
  io.println("\(r)");
  <0
};
EOF
expect "3-sum-arm-binding-E4070" "$(errs "${WORK}/p3.tk")" "E4070 "

# 4. a closure parameter that shadows an outer `mut` is still the parameter
cat > "${WORK}/p4.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let a=mut.1;
  let g=fn(a:i64):i64{ a=a*10; <a };
  io.println("\(g(2))");
  <0
};
EOF
expect "4-param-shadowing-mut-E4070" "$(errs "${WORK}/p4.tk")" "E4070 "

# 5. guard: copy the parameter into a `let x=mut.` and assign that
cat > "${WORK}/p5.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let g=fn(a:i64):i64{ let b=mut.a; b=b+1; <b };
  io.println("\(g(4))");
  <0
};
EOF
expect "5-copied-param-checks" "$(errs "${WORK}/p5.tk")" ""
expect "5-copied-param-runs"   "$(runs "${WORK}/p5.tk")" "5 "

# 6. guard: an arm that rebinds its binding with `let v=mut.v` may assign it
cat > "${WORK}/p6.tk" <<'EOF'
m=t;
i=io:std.io;
t=$e{$bad:i64};
f=pick(x:i64):i64!$e{ if(x>0){<x}; <$e{$bad:x} };
f=main():i64{
  let k=mt pick(3){$ok:v if(v>0){let v=mut.v; v=v+1; v}el{v};$err:e 0};
  io.println("\(k)");
  <0
};
EOF
expect "6-rebound-arm-binding-checks" "$(errs "${WORK}/p6.tk")" ""
expect "6-rebound-arm-binding-runs"   "$(runs "${WORK}/p6.tk")" "4 "

# 7. guard: the outer `mut` a closure parameter shadows stays assignable
cat > "${WORK}/p7.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let a=mut.1;
  let g=fn(a:i64):i64{ <a*10 };
  a=a+g(2);
  io.println("\(a)");
  <0
};
EOF
expect "7-outer-mut-checks" "$(errs "${WORK}/p7.tk")" ""
expect "7-outer-mut-runs"   "$(runs "${WORK}/p7.tk")" "21 "

# 8. guard: a closure assigning a captured `mut` is unchanged (by-value capture)
cat > "${WORK}/p8.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let n=mut.1;
  let g=fn(x:i64):i64{ n=n+x; <n };
  io.println("\(g(2))");
  io.println("\(n)");
  <0
};
EOF
expect "8-captured-mut-checks" "$(errs "${WORK}/p8.tk")" ""
expect "8-captured-mut-runs"   "$(runs "${WORK}/p8.tk")" "3 1 "

# 9. guard: an immutable `let` still carries its fix; a function parameter still E4070
cat > "${WORK}/p9.tk" <<'EOF'
m=t;
f=main():i64{ let x=1; x=2; <x };
EOF
expect "9-let-fix-kept" "$(hasfix "${WORK}/p9.tk")" "yes"
cat > "${WORK}/p10.tk" <<'EOF'
m=t;
f=bad(x:i64):i64{ x=2; <x };
f=main():i64{ <bad(1) };
EOF
expect "9-fn-param-E4070" "$(errs "${WORK}/p10.tk")" "E4070 "

echo ""
echo "C729: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
