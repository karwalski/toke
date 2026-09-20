#!/usr/bin/env bash
# C009_void_err_union_arms.sh — `mt` over a `void!$err` call picks the arm the
# CALL decided, not a constant (story 127.95).
#
# `void!$err` is not void.  tki_base_return_type() stripped the error union,
# the remaining base "void" mapped to an LLVM void return, and the call was
# lowered as `call void @tk_sse_emit_w(...)` — discarding the status word the
# wrapper had just returned — followed by
#
#     %t = add i64 0, 0 ; void call result
#     %t' = icmp ne i64 %t, 0
#     br i1 %t', label %rm_ok, label %rm_err
#
# The test is against a literal zero, so the $err arm was taken
# UNCONDITIONALLY, whatever happened.  That is every `mt sse.emit(...)` and
# every `mt ws.send(...)` in the language: error handling on the two streaming
# paths has never worked and has always reported failure, silently, at exit 0.
#
# It is 127.80's and 127.86's shape a third time — a default substituted for
# an answer the compiler had and threw away.  The interface file says
# `"return": "void!sseerr"`; only the half before the `!` was read.
#
# The wrappers agree on the convention (sse_glue.c, ws_glue.c, router_glue.c,
# tk_web_glue.c): 0 is success and -1 is failure — the INVERSE of the 0/null
# sentinel every valued error union uses, which is why the arm test for this
# one shape is `== 0`.
#
# Cases below assert the ARM TAKEN by running the program, for a call that
# succeeds and for the same call that fails, plus the IR shape that made it
# unobservable, plus controls that must keep compiling and keep their own
# sentinel.
#
# Story: 127.95

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_voiderr_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C009: mt over void!\$err takes the arm the call decided"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE
#   Compiles AND RUNS.  Only a run can prove which arm was taken: the wrong
#   arm compiles exactly as happily as the right one, which is the defect.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > run.tk
    local out rc=0
    rm -f run.bin
    out="$("${TKC}" --allow-all --diag-json -o run.bin run.tk 2>&1)" || rc=$?
    if [ ! -x run.bin ]; then
        echo "  FAIL: ${name}: did not compile (exit ${rc})"
        echo "${out}" | sed 's/^/      /' | head -3
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
    if grep -q "${pattern}" ir.ll; then
        echo "  FAIL: ${name}: IR still contains [${pattern}]"
        grep -n "${pattern}" ir.ll | sed 's/^/      /' | head -3
        rm -f ir.ll
        FAIL=$((FAIL + 1))
        return
    fi
    rm -f ir.ll
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ir_present NAME PATTERN SOURCE — the emitted IR MUST contain PATTERN.
ir_present() {
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
    if ! grep -q "${pattern}" ir.ll; then
        echo "  FAIL: ${name}: IR did not contain [${pattern}]"
        rm -f ir.ll
        FAIL=$((FAIL + 1))
        return
    fi
    rm -f ir.ll
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ── The reported case.  `sse.emit` on an OPEN context writes the event block
#    and returns 0.  The old compiler printed the block (so the call plainly
#    worked) and then took the $err arm anyway.  The event body is part of the
#    expected output precisely because it proves the call succeeded. ──
run_case "sse.emit that succeeds takes the \$ok arm" \
    "$(printf 'event: update\ndata: hi\nid: 1\n\nOK-arm')" '
m=t;
i=io:std.io;
i=sse:std.sse;
f=main():i64{
  let ctx=$ssectx{id:1;open:true};
  let ev=$sseevent{id:"1";event:"update";data:"hi";retry:0};
  mt sse.emit(ctx;ev) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── The other direction, which is what makes the fix a fix and not an
#    inversion: a call that genuinely fails must still reach $err.  A closed
#    context is refused by the wrapper before any write. ──
run_case "sse.emit on a closed context takes the \$err arm" \
    "ERR-arm" '
m=t;
i=io:std.io;
i=sse:std.sse;
f=main():i64{
  let closed=$ssectx{id:1;open:false};
  let ev=$sseevent{id:"1";event:"update";data:"hi";retry:0};
  mt sse.emit(closed;ev) {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── sse.emitdata, the second void!$err entry point on the same page. ──
run_case "sse.emitdata that succeeds takes the \$ok arm" \
    "$(printf 'data: ping\n\nOK-arm')" '
m=t;
i=io:std.io;
i=sse:std.sse;
f=main():i64{
  let ctx=$ssectx{id:1;open:true};
  mt sse.emitdata(ctx;"ping") {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

run_case "sse.emitdata on a closed context takes the \$err arm" \
    "ERR-arm" '
m=t;
i=io:std.io;
i=sse:std.sse;
f=main():i64{
  let closed=$ssectx{id:1;open:false};
  mt sse.emitdata(closed;"ping") {$ok:v io.println("OK-arm");$err:e io.println("ERR-arm")};
  <0
};'

# ── The arm must also be right when the match is USED AS AN EXPRESSION, which
#    is how a handler normally spells it (`let ok = mt ws.send(...) {...}`). ──
run_case "the arm is right when the match is an expression" \
    "$(printf 'event: e\ndata: d\nid: i\n\n1')" '
m=t;
i=io:std.io;
i=s:std.str;
i=sse:std.sse;
f=main():i64{
  let ctx=$ssectx{id:1;open:true};
  let ev=$sseevent{id:"i";event:"e";data:"d";retry:0};
  let ok=mt sse.emit(ctx;ev) {$ok:v 1;$err:e 0};
  io.println(s.fromint(ok));
  <0
};'

# ── The IR shape that made all of the above unobservable.  The call is no
#    longer a void call and no longer discards its result, so there is nothing
#    left to substitute a literal for. ──
ir_absent "the sse.emit call is not lowered as a void call" \
    "call void @tk_sse_emit_w" '
m=t;
i=sse:std.sse;
f=main():i64{
  let ctx=$ssectx{id:1;open:true};
  let ev=$sseevent{id:"1";event:"u";data:"d";retry:0};
  mt sse.emit(ctx;ev) {$ok:v 1;$err:e 0};
  <0
};'

ir_absent "no literal zero stands in for the status word" \
    "add i64 0, 0 ; void call result" '
m=t;
i=sse:std.sse;
f=main():i64{
  let ctx=$ssectx{id:1;open:true};
  let ev=$sseevent{id:"1";event:"u";data:"d";retry:0};
  mt sse.emit(ctx;ev) {$ok:v 1;$err:e 0};
  <0
};'

# ── ws.send is the other shipped void!$err path.  std.ws needs a live socket
#    to run, so it is pinned on the IR: the call must carry its result and the
#    arm test must be the `== 0` this convention uses, not `!= 0`. ──
ir_present "ws.send carries its status word into the arm test" \
    "call i64 @tk_ws_send_w" '
m=t;
i=ws:std.ws;
f=main():i64{
  let conn=$wsconn{id:1;ready:true};
  mt ws.send(conn;"hello") {$ok:v 1;$err:e 0};
  <0
};'

ir_present "ws.send discriminates on 0 = ok, not 0 = err" \
    "127.95 void!\$err" '
m=t;
i=ws:std.ws;
f=main():i64{
  let conn=$wsconn{id:1;ready:true};
  mt ws.send(conn;"hello") {$ok:v 1;$err:e 0};
  <0
};'

# ── Controls.  A VALUED error union keeps the 0/null sentinel — inverting it
#    for everything would have been a far larger defect than the one fixed. ──
run_case "control: a valued \$str!\$err still takes \$ok on success" \
    "hello" '
m=t;
i=io:std.io;
i=file:std.file;
f=main():i64{
  file.write("c009ctl.txt";"hello");
  let s=mt file.read("c009ctl.txt") {$ok:v v;$err:e "READ-FAILED"};
  io.println(s);
  <0
};'

run_case "control: a valued \$str!\$err still takes \$err on failure" \
    "READ-FAILED" '
m=t;
i=io:std.io;
i=file:std.file;
f=main():i64{
  let s=mt file.read("c009_no_such_file.txt") {$ok:v v;$err:e "READ-FAILED"};
  io.println(s);
  <0
};'

run_case "control: an i64!\$err whose ok value IS zero still takes \$ok" \
    "0" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let n=mt s.toint("0") {$ok:v v;$err:e 0-1};
  io.println(s.fromint(n));
  <0
};'

# Control: a plain `void` stdlib call — no error union — must stay a void call.
# The change is scoped to `void!<E>`; widening it to bare void would give every
# sse.close a phantom status word.
ir_present "control: a plain void call stays a void call" \
    "call void @tk_sse_close_w" '
m=t;
i=sse:std.sse;
f=main():i64{
  let ctx=$ssectx{id:1;open:true};
  sse.close(ctx);
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
