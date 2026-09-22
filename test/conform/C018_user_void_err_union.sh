#!/usr/bin/env bash
# C018_user_void_err_union.sh — a USER-DECLARED fallible void function compiles,
# and `mt` over it takes the arm the call decided (story 127.111).
#
# `f=doit(n:i64):void!$myerr{...}` could not be compiled AT ALL.  The $ok arm's
# binding was emitted from the scrutinee's declared type, which for a void
# return is the literal LLVM `void`:
#
#     %v = alloca void
#     store void %t16, void* %v
#
# and clang refused it — "void type only allowed for function results" —
# surfacing as an opaque E9003 from the codegen stage.  So the shape the
# standard library declares seven times over (sse.emit, sse.emitdata, ws.send,
# …) could not be written in toke.
#
# 127.95 fixed only the IMPORTED spelling, because a stdlib `void!$err`
# resolves through the interface cache — which maps it to the i64 status word
# the wrapper returns — while a locally declared one goes through the callee's
# own return type and stayed `void`.  Two paths, one fixed.
#
# The call path already synthesises an i64 filler for a void call result
# ("void call result"), so the scrutinee VALUE was always an i64 while its
# reported TYPE was void.  The fix normalises the type to the filler's real
# type, which also makes `$ok:v` mean the same thing on both paths: v binds the
# i64 filler locally, exactly as it binds the i64 status word on the imported
# path.
#
# It failed loudly, so nothing was banked wrong — but it is a total blocker for
# anyone writing a fallible void function.
#
# Story: 127.111

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The tkc symlink is relinked by any concurrent `make`; $TKC lets a caller pin
# a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c018_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C018: a user-declared void!\$err function compiles and dispatches"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE — compiles AND RUNS.  Only a run proves
# which arm was taken; the wrong arm compiles exactly as happily as the right
# one, and before the fix NEITHER compiled.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > run.tk
    local out rc=0
    rm -f run.bin
    out="$("${TKC}" --allow-all --diag-json -o run.bin run.tk 2>&1)" || rc=$?
    if [ ! -x run.bin ]; then
        echo "  FAIL: ${name}: did not compile (exit ${rc})"
        echo "${out}" | grep -v '"severity":"warning"' | sed 's/^/      /' | head -4
        FAIL=$((FAIL + 1))
        return
    fi
    local got
    got="$(./run.bin 2>&1)"
    rm -f run.bin
    if [ "${got}" != "${expected}" ]; then
        echo "  FAIL: ${name}: expected [${expected}], got [${got}]"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ir_absent NAME PATTERN SOURCE — the emitted IR must NOT contain PATTERN.
ir_absent() {
    local name="$1" pattern="$2" src="$3"
    printf '%s\n' "${src}" > ir.tk
    local rc=0
    rm -f ir.ll
    "${TKC}" --allow-all --diag-json --emit-llvm -o ir.ll ir.tk >/dev/null 2>&1 || rc=$?
    if [ ! -f ir.ll ]; then
        echo "  FAIL: ${name}: no IR emitted (exit ${rc})"
        FAIL=$((FAIL + 1))
        return
    fi
    if grep -q -- "${pattern}" ir.ll; then
        echo "  FAIL: ${name}: IR still contains [${pattern}]"
        grep -n -- "${pattern}" ir.ll | sed 's/^/      /' | head -3
        rm -f ir.ll
        FAIL=$((FAIL + 1))
        return
    fi
    rm -f ir.ll
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ── The reported case: a RECORD-style error type, which is what almost all of
#    the standard library and all of ooke declares. ──
run_case "record-typed void!\$err: the success path reaches \$ok" \
    "$(printf 'did it\nOK-arm')" '
m=t;
i=io:std.io;
t=$myerr{code:i64;msg:$str};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1;msg:"neg"}};
  io.println("did it")
};
f=main():i64{
  mt doit(1) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── The other direction, which is what makes the fix a fix and not a constant:
#    the error path must reach $err AND the payload must be readable there
#    (127.97 gave the box a payload; a void-returning callee must carry it
#    exactly like a valued one). ──
run_case "record-typed void!\$err: the error path reaches \$err with its payload" \
    "neg" '
m=t;
i=io:std.io;
t=$myerr{code:i64;msg:$str};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:7;msg:"neg"}};
  io.println("did it")
};
f=main():i64{
  mt doit(0-1) {$ok:v io.println("OK-arm");$err:e io.println(e.msg)};
  <0
};'

run_case "the error payload's numeric field is readable from a void callee" \
    "7" '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64;msg:$str};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:7;msg:"neg"}};
  io.println("did it")
};
f=main():i64{
  mt doit(0-1) {$ok:v io.println("OK-arm");$err:e io.println(s.fromint(e.code))};
  <0
};'

# ── A SUM-style error type on the same shape.  114.41's box, not 127.97's. ──
run_case "sum-typed void!\$err: both arms are reachable" \
    "$(printf 'ERR-arm\ndid it\nOK-arm')" '
m=t;
i=io:std.io;
t=$sumerr{$bad:bool;$worse:i64};
f=doit(n:i64):void!$sumerr{
  if(n<0){<$sumerr{$bad:true}};
  io.println("did it")
};
f=main():i64{
  mt doit(0-1) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  mt doit(1)   {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── 127.112, found by fixing 127.111 and strictly worse than it.  A fallible
#    void function has no `<` on its success path, so the implicit return never
#    cleared the slot (114.55 covered only the explicit `<expr` paths).  The
#    second call below plainly SUCCEEDS — it prints "did it" — and used to
#    report failure anyway, because the FIRST call's error was still in the
#    slot.  Order-dependent and silent, where 127.111 was loud. ──
run_case "a successful void!\$err call after an earlier error still reaches \$ok" \
    "$(printf 'ERR-arm\ndid it\nOK-arm')" '
m=t;
i=io:std.io;
t=$myerr{code:i64;msg:$str};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1;msg:"neg"}};
  io.println("did it")
};
f=main():i64{
  mt doit(0-1) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  mt doit(1)   {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── The same hole on a bare `<` (a valueless explicit return). ──
run_case "a bare \`<\` is an ok return and clears the slot too" \
    "$(printf 'ERR-arm\nOK-arm')" '
m=t;
i=io:std.io;
t=$myerr{code:i64};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1}};
  <
};
f=main():i64{
  mt doit(0-1) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  mt doit(1)   {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── The arm must also be right when the match is USED AS AN EXPRESSION. ──
run_case "the arm is right when the match is an expression" \
    "$(printf 'did it\n1')" '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1}};
  io.println("did it")
};
f=main():i64{
  let ok=mt doit(1) {$ok:v 1;$err:e 0};
  io.println(s.fromint(ok));
  <0
};'

# ── An $ok arm that BINDS a name is the exact expression that produced
#    `alloca void`.  Binding and then using the binding must both work. ──
run_case "an \$ok arm that binds and uses its name compiles" \
    "$(printf 'did it\nbound=0')" '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1}};
  io.println("did it")
};
f=main():i64{
  mt doit(1) {$ok:v io.println("bound=\(v)");$err:e io.println("ERR-arm")};
  <0
};'

# ── The IR shape that made it uncompilable.  There is no void alloca and no
#    void store anywhere in the module. ──
ir_absent "no \`alloca void\` is emitted for the \$ok binding" \
    "alloca void" '
m=t;
i=io:std.io;
t=$myerr{code:i64};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1}};
  io.println("did it")
};
f=main():i64{
  mt doit(1) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

ir_absent "no \`store void\` is emitted for the \$ok binding" \
    "store void" '
m=t;
i=io:std.io;
t=$myerr{code:i64};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1}};
  io.println("did it")
};
f=main():i64{
  mt doit(1) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── Controls.  The change is scoped to the `mt` scrutinee's type; a plain
#    `void` function with no error union must keep returning void, and a
#    VALUED error union must keep its own value in the $ok arm. ──
run_case "control: a plain void function (no error union) still works" \
    "$(printf 'plain\n0')" '
m=t;
i=io:std.io;
i=s:std.str;
f=plain(n:i64):void{ io.println("plain") };
f=main():i64{
  plain(1);
  io.println(s.fromint(0));
  <0
};'

run_case "control: a valued i64!\$err still binds its real value in \$ok" \
    "42" '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64};
f=valued(n:i64):i64!$myerr{
  if(n<0){<$myerr{code:1}};
  <42
};
f=main():i64{
  let r=mt valued(1) {$ok:v v;$err:e 0-1};
  io.println(s.fromint(r));
  <0
};'

run_case "control: a valued \$str!\$err still binds its real value in \$ok" \
    "hello" '
m=t;
i=io:std.io;
t=$myerr{code:i64};
f=valued(n:i64):$str!$myerr{
  if(n<0){<$myerr{code:1}};
  <"hello"
};
f=main():i64{
  let r=mt valued(1) {$ok:v v;$err:e "ERR"};
  io.println(r);
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
