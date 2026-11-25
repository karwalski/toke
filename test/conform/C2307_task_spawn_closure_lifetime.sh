#!/usr/bin/env bash
# C2307_task_spawn_closure_lifetime.sh — a closure handed to task.spawn is
# called as a closure on the pool thread, and the task holds a reference to
# its environment until it has run (story 127.549).
#
# THE DEFECT: a closure value is its env box + 1 (124.0c).  task.spawn
# handed it to the runtime as if it were a function address and the pool
# thread jumped to it: `let cl=fn():bool{<k>5}; task.spawn(s;cl)` died with
# SIGBUS on arm64 at main 58bd74e8 (a literal did not even build).
# Nothing tied a closure's lifetime to a task that runs after the spawning
# statement, on another thread: the reference a local's read took was never
# released (a leak).
#
# THE FIX: the adapter tests the closure tag as the toke-side call does.
# The task OWNS one reference: the compiler hands the value over +1 (a
# literal, or a call returning a closure, is already owned; a closure-bound
# local or a parameter is retained at the call), and task.c releases it once
# the function has returned (tk_clos_release; the count is atomic).  C2308
# checks the runtime half in C.
#
# WHAT THIS CHECKS:
#   1. IR: a closure literal goes to tk_task_spawnfv as the closure box and
#      the caller does NOT release it (the task does); a closure-bound local
#      is retained right before the call;
#   2. runs: literal, capturing literal, closure-bound local (twice, then
#      still callable here), a closure through a ():bool parameter, a
#      closure returned by a function, the `spawn` keyword with a closure,
#      and 300 capturing closure tasks over three scopes (the environment
#      of each is released by its worker after it ran).
#
# Negative control: main 58bd74e8 fails both: the IR has no
# tk_task_spawnfv, and the program does not build (a closure literal's i8*
# went where tk_task_spawn takes an i64: ill-typed IR, E9003).  A program
# passing closure-bound locals does build there and dies with SIGBUS.
#
# Story: 127.549
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2307_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2307: closures handed to task.spawn: called as closures, kept alive by the task (127.549)"
echo "----------------------------------------------------------------------------------------"

cat > ir.tk <<'TOKE'
m=q;
i=task:std.task;
f=main():i64{
  let s=task.scope();
  let k=4;
  let a=task.spawn(s;fn():i64{<k+1});
  let cl=fn():bool{<k>2};
  let b=task.spawn(s;cl);
  <task.awaitall(s)
};
TOKE

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=task:std.task;
f=mk(k:i64):():i64{<fn():i64{<k*3}};
f=spawnit(s:i64;g:():bool):i64{<task.spawn(s;g)};
f=batch(base:i64):i64{
  let s=task.scope();
  let hs=mut.@();
  lp(let i=0;i<100;i=i+1){
    let v=base+i;
    hs=hs.push(task.spawn(s;fn():i64{<v*2}))
  };
  let rc=task.awaitall(s);
  let tot=mut.0;
  lp(let i=0;i<100;i=i+1){tot=tot+task.result(hs.get(i))};
  <tot
};
f=main():i64{
  let s=task.scope();
  let k=7;
  let a=task.spawn(s;fn():i64{<41});
  let b=task.spawn(s;fn():i64{<k+1});
  let cl=fn():bool{<k>5};
  let c=task.spawn(s;cl);
  let d=task.spawn(s;cl);
  let e=spawnit(s;fn():bool{<k<5});
  let g=task.spawn(s;mk(10));
  let rc=task.awaitall(s);
  let r=mut.@(0);
  sc{ r=r.set(0;spawn fn():i64{<k*k}) };
  let still=if(cl()){1}el{0};
  io.println("\(task.result(a)) \(task.result(b)) \(task.result(c)) \(task.result(d)) \(task.result(e)) \(task.result(g)) \(task.result(r.get(0))) \(still)");
  io.println("\(batch(0)+batch(1000)+batch(2000))");
  <0
};
TOKE

if "${TKC}" --emit-llvm --out ir.ll ir.tk >emit.err 2>&1; then
  r=$(python3 - ir.ll <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
main = re.search(r"^define [^\n]*@tk_main\([^\n]*\{(.*?)^\}", ir, re.M | re.S)
bad = []
if not main: bad.append("no tk_main")
else:
    body = main.group(1)
    calls = re.findall(r"call i64 @tk_task_spawnfv\(([^)]*)\)", body)
    if len(calls) != 2: bad.append("expected 2 tk_task_spawnfv calls, found %d" % len(calls))
    else:
        lit = calls[0].split(",")[2].strip().split()[-1]
        if not re.search(re.escape(lit) + r" = ptrtoint i8\* %t\d+ to i64", body):
            bad.append("1st call's value is not the closure box")
        # the box the literal built: no release of any ptrtoint of it in main
        box = re.search(re.escape(lit) + r" = ptrtoint i8\* (%t\d+) to i64", body)
        if box:
            regs = [lit] + re.findall(r"(%t\d+) = ptrtoint i8\* " + re.escape(box.group(1)) + r" to i64", body)
            for rg in regs:
                if re.search(r"call void @tk_clos_release\(i64 " + re.escape(rg) + r"\)", body):
                    bad.append("the caller releases the literal it handed to the task")
        loc = calls[1].split(",")[2].strip().split()[-1]
        pre = body.split("call i64 @tk_task_spawnfv(" + calls[1], 1)[0].rsplit("@tk_task_spawnfv", 1)[-1]
        if "call void @tk_clos_retain" not in pre:
            bad.append("the closure-bound local is not retained for the task")
print("\n".join(bad) if bad else "OK")
PY
)
  [ "$r" = OK ] && pass "IR: literal handed over owned, local retained for the task" || fail "IR: $r"
else fail "emit-llvm failed: $(head -c 300 emit.err)"; fi

if "${TKC}" --allow-all --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want=$'41 8 1 1 0 30 49 1\n629700'
  [ "$rc" -eq 0 ] && [ "$out" = "$want" ] && pass "runs: literal, capture, local x2, param, returned, spawn kw, 300 tasks" || fail "runs: rc=$rc out=$(echo $out)"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2307: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
