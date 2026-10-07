#!/usr/bin/env bash
# C723_fnvalue_error_channel.sh — `!` and `mt` on a call through a function
# VALUE decide on the callee's error channel (story 127.276).
#
# THE DEFECT.  Codegen tracked the error channel (@tk_current_error) only for
# NAMED callees.  A call through a function value — `let g=inner`, a copy of
# `&inner`, a closure, an array element, an immediately-applied closure — fell
# back to the value sentinel, so on main:
#     let g=inner; let v=g(5)!$ea      -> takes $ok, prints "ok 6"  (inner failed)
#     let v=g(0)!$ea                   -> takes $err (an ok value of 0)
#     mt g(5){$ok..;$err:e mt e{..}}   -> E9005 at build (error type unknown)
#     a T!$E closure                   -> returned its error box's ADDRESS as
#                                         the ok value ("ok 4379859649")
# and a mismatched error type through a function value was neither E4076 nor
# RT010 (127.225 judged named callees only).  A fallible function stored in a
# struct field declared `(i64):i64` was accepted, and the call through it read
# the error-exit filler as an ok value.
#
# NOW.  The checker records a function value's signature, error type
# included (`let g=inner` too, as `&inner` already was); codegen keys the
# channel and the $err payload type on it; a `T!$E` closure keeps the
# channel on every exit (127.185's rule, now checked for closures too).
# A fallible function in a slot declared infallible is E4031, and the
# signature prints its error type ("(i64):i64!$ea", not "(i64):i64").
#
# NOT COVERED: a parameter or struct field cannot be DECLARED fallible —
# FuncTypeExpr has no `!` part (grammar.ebnf) — so cases 7 and 8 assert the
# rejection, not propagation.  Extending the grammar is an owner decision.
#
# Red on main (e9ec45b5): every case but the controls (12-14) fails.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc not found at ${TKC}"; exit 1; fi
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
codes() { (cd "${WORK}" && "${TKC}" --check "$1" 2>&1) | grep -o '"error_code":"[A-Z0-9]*"' | sed 's/.*:"//;s/"//' | tr '\n' ' '; }
runs()  { (cd "${WORK}" && rm -f b && "${TKC}" "$1" -o b >/dev/null 2>&1 && ./b 2>&1; echo "rc=$?") | tr '\n' ' '; }
# stdout, then stderr, then the exit status (stdout is buffered past a trap)
runs2() { (cd "${WORK}" && rm -f b && "${TKC}" "$1" -o b >/dev/null 2>&1 && ./b >o 2>e; rc=$?; cat o e 2>/dev/null; echo "rc=${rc}") | tr '\n' ' '; }

HDR='m=t;i=io:std.io;
t=$ea{$x:i64;$y:str};
t=$eb{$p:str;$q:i64};
f=inner(n:i64):i64!$ea{if(n>0){<$ea{$y:"boom"}};<n};'
# outer(5) fails, outer(0) succeeds with the ok value 0 (+1).
MAIN='f=main():i64{
  mt outer(5){$ok:v io.println("ok \(v)");$err:e mt e{$x:k io.println("x \(k)");$y:s io.println("y \(s)")}};
  mt outer(0){$ok:v io.println("ok \(v)");$err:e io.println("err")};
  <0
};'

# ── 1. let-bound module function, `!` ─────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$ea{let g=inner;let v=g(n)!$ea;<v+1};\n%s\n' "${HDR}" "${MAIN}" > "${WORK}/p1.tk"
expect "1-let-bound-propagates" "$(runs p1.tk)" "y boom ok 1 rc=0 "

# ── 2. let-bound, `mt` directly on the call ───────────────────────────
cat > "${WORK}/p2.tk" <<EOF
${HDR}
f=main():i64{
  let g=inner;
  mt g(5){\$ok:v io.println("ok \(v)");\$err:e mt e{\$x:k io.println("x \(k)");\$y:s io.println("y \(s)")}};
  mt g(0){\$ok:v io.println("ok \(v)");\$err:e io.println("err")};
  <0
};
EOF
expect "2-let-bound-mt" "$(runs p2.tk)" "y boom ok 0 rc=0 "

# ── 3. closure, `!` and `mt` ──────────────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$ea{let g=fn(k:i64):i64!$ea{if(k>0){<$ea{$y:"cl"}};<k};let v=g(n)!$ea;<v+1};\n%s\n' "${HDR}" "${MAIN}" > "${WORK}/p3.tk"
expect "3-closure-propagates" "$(runs p3.tk)" "y cl ok 1 rc=0 "
cat > "${WORK}/p3b.tk" <<EOF
${HDR}
f=main():i64{
  let g=fn(k:i64):i64!\$ea{if(k>0){<\$ea{\$x:k}};<k};
  mt g(4){\$ok:v io.println("ok \(v)");\$err:e mt e{\$x:k io.println("x \(k)");\$y:s io.println("y \(s)")}};
  mt g(0){\$ok:v io.println("ok \(v)");\$err:e io.println("err")};
  <0
};
EOF
expect "3-closure-mt" "$(runs p3b.tk)" "x 4 ok 0 rc=0 "

# ── 4. a copy of a function reference ─────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$ea{let r=&inner;let g=r;let v=g(n)!$ea;<v+1};\n%s\n' "${HDR}" "${MAIN}" > "${WORK}/p4.tk"
expect "4-fref-copy" "$(runs p4.tk)" "y boom ok 1 rc=0 "

# ── 5. an array element ───────────────────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$ea{let a=@(fn(k:i64):i64!$ea{if(k>0){<$ea{$y:"arr"}};<k});let g=a.get(0);let v=g(n)!$ea;<v+1};\n%s\n' "${HDR}" "${MAIN}" > "${WORK}/p5.tk"
expect "5-array-element" "$(runs p5.tk)" "y arr ok 1 rc=0 "

# ── 6. an immediately-applied closure ─────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$ea{let v=(fn(k:i64):i64!$ea{if(k>0){<$ea{$y:"iife"}};<k})(n)!$ea;<v+1};\n%s\n' "${HDR}" "${MAIN}" > "${WORK}/p6.tk"
expect "6-iife" "$(runs p6.tk)" "y iife ok 1 rc=0 "

# ── 7. struct field declared infallible: a fallible value is E4031 ────
for v in 'inner' '&inner' 'fn(k:i64):i64!$ea{<k}'; do
  # types precede functions
  cat > "${WORK}/p7.tk" <<EOF
m=t;i=io:std.io;
t=\$ea{\$x:i64;\$y:str};
t=\$h{f:(i64):i64};
f=inner(n:i64):i64!\$ea{if(n>0){<\$ea{\$y:"boom"}};<n};
f=main():i64{let h=\$h{f:${v}};let k=h.f;io.println("\(k(5))");<0};
EOF
  rec="$(cd "${WORK}" && "${TKC}" --check p7.tk 2>&1 | grep E4031)"
  got=""
  case "${rec}" in *"\"got\":\"(i64):i64!\$ea\""*) got="names-error-type";; esac
  case "${rec}" in *'"fix"'*) got="${got} HAS-FIX";; esac
  expect "7-struct-field-${v%%(*}" "$(codes p7.tk)|${got}" "E4031 |names-error-type"
done

# ── 8. parameter declared infallible: E4031 names the error type ──────
printf '%s\nf=ap(g:(i64):i64;n:i64):i64{<g(n)};\nf=main():i64{<ap(inner;5)};\n' "${HDR}" > "${WORK}/p8.tk"
rec="$(cd "${WORK}" && "${TKC}" --check p8.tk 2>&1 | grep E4031)"
case "${rec}" in *"got '(i64):i64!\$ea'"*) m8="names-error-type";; *) m8="${rec:0:160}";; esac
expect "8-parameter-E4031" "$(codes p8.tk)|${m8}" "E4031 |names-error-type"

# ── 9. mismatched error type, checker can see it: E4076 ───────────────
printf '%s\nf=outer(n:i64):i64!$eb{let g=inner;let v=g(n)!$eb;<v+1};\nf=main():i64{<0};\n' "${HDR}" > "${WORK}/p9.tk"
expect "9-let-bound-E4076" "$(codes p9.tk)" "E4076 "
printf '%s\nf=outer(n:i64):i64!$eb{let g=fn(k:i64):i64!$ea{<k};let v=g(n)!$eb;<v+1};\nf=main():i64{<0};\n' "${HDR}" > "${WORK}/p9b.tk"
expect "9-closure-E4076" "$(codes p9b.tk)" "E4076 "

# ── 10. mismatched layouts the checker cannot judge: RT010 ────────────
RHDR='m=t;i=io:std.io;
t=$ra{a:$none};
t=$rb{a:i64};
f=inner(n:i64):i64!$ra{if(n>0){<$ra{a:7}};<n};'
RMAIN='f=main():i64{
  mt outer(0){$ok:v io.println("ok \(v)");$err:e io.println("err")};
  mt outer(5){$ok:v io.println("ok \(v)");$err:e io.println("err")};
  <0
};'
RT="RT010: error of type \$ra re-raised with '!' into a function whose error type is \$rb"
printf '%s\nf=outer(n:i64):i64!$rb{let g=inner;let v=g(n)!$rb;<v+1};\n%s\n' "${RHDR}" "${RMAIN}" > "${WORK}/p10.tk"
expect "10-let-bound-checks-clean" "$(codes p10.tk)" ""
expect "10-let-bound-RT010" "$(runs2 p10.tk)" "ok 1 ${RT} rc=1 "
printf '%s\nf=outer(n:i64):i64!$rb{let g=fn(k:i64):i64!$ra{if(k>0){<$ra{a:7}};<k};let v=g(n)!$rb;<v+1};\n%s\n' "${RHDR}" "${RMAIN}" > "${WORK}/p10b.tk"
expect "10-closure-RT010" "$(runs2 p10b.tk)" "ok 1 ${RT} rc=1 "

# ── 11. a scalar error type: the payload is carried ───────────────────
cat > "${WORK}/p11.tk" <<'EOF'
m=t;i=io:std.io;
f=inner(n:i64):i64!str{if(n>0){<$err("bad")};<n};
f=outer(n:i64):i64!str{let g=inner;let v=g(n)!str;<v+1};
f=main():i64{
  let g=inner;
  mt g(5){$ok:v io.println("ok \(v)");$err:e io.println("err \(e)")};
  mt outer(5){$ok:v io.println("ok \(v)");$err:e io.println("err \(e)")};
  mt outer(0){$ok:v io.println("ok \(v)");$err:e io.println("err \(e)")};
  <0
};
EOF
expect "11-str-error" "$(runs p11.tk)" "err bad err bad ok 1 rc=0 "

# ── 12-14. controls: infallible function values are unchanged ─────────
cat > "${WORK}/p12.tk" <<'EOF'
m=t;i=io:std.io;
t=$h{f:(i64):i64};
f=twice(n:i64):i64{<n*2};
f=main():i64{
  let g=twice;
  let c=fn(k:i64):i64{<k+1};
  let h=$h{f:twice};
  let hf=h.f;
  io.println("\(g(5)) \(c(5)) \(hf(5)) \(g(0))");
  <0
};
EOF
expect "12-infallible-checks" "$(codes p12.tk)" ""
expect "12-infallible-runs" "$(runs p12.tk)" "10 6 10 0 rc=0 "
# a named callee is untouched
printf '%s\nf=outer(n:i64):i64!$ea{let v=inner(n)!$ea;<v+1};\n%s\n' "${HDR}" "${MAIN}" > "${WORK}/p13.tk"
expect "13-named-callee" "$(runs p13.tk)" "y boom ok 1 rc=0 "
# arity through `let g=inner` is now judged against inner's signature
printf '%s\nf=main():i64{let g=inner;mt g(1;2){$ok:v v;$err:e 0};<0};\n' "${HDR}" > "${WORK}/p14.tk"
expect "14-let-bound-arity" "$(codes p14.tk)" "E4026 "

echo ""
echo "C723: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
