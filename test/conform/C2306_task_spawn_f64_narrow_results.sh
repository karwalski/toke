#!/usr/bin/env bash
# C2306_task_spawn_f64_narrow_results.sh — task.result of an f64, a narrow
# int and a str task is the value the function returned, widened to a
# defined i64 (story 127.549).
#
# THE DEFECT: the pool thread called the bare value as `int64_t (*)(void)`
# and stored the integer return register.  An f64 function returns in a
# float register (d0 / xmm0) that C never read, so task.result of
# `f=half():f64{<1.5}` was whatever x0/rax held -- 0 on arm64, on EVERY
# target wrong; a narrow int's upper bits were whatever the callee left.
#
# THE FIX: the adapter widens the result the way the 127.548 combinator
# adapters do: the bits of a double (math.frombits reads them back), sext of
# a signed narrow int, zext of an unsigned one; a str comes back as the
# pointer it is.
#
# WHAT THIS CHECKS:
#   1. IR (host and x86_64): the adapter for the f64 task calls the value
#      `fastcc double` and bitcasts it to i64; the i32 one sexts, the u16 one
#      zexts; the str one calls `fastcc i64` and returns it;
#   2. runs: math.frombits(task.result(f64 task)) is 1.5, a closure
#      returning f64 0.25, i32 -5 reads -5, u16 65000 reads 65000, and the
#      str task's result is the address of a string (non-zero).
#
# Negative control: main 58bd74e8 fails all three: no adapter on either
# target, and its run dies with SIGBUS (exit 138) calling the closure as
# code.  Without the closure its run prints 0 for the f64 task.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2306_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2306: f64 / narrow int / str task results through the C-ABI adapter (127.549)"
echo "-------------------------------------------------------------------------------"

cat > ir.tk <<'TOKE'
m=p;
i=task:std.task;
f=half():f64{<1.5};
f=n32():i32{<-5};
f=u16v():u16{<65000};
f=nm():str{<"hello"};
f=main():i64{
  let s=task.scope();
  let a=task.spawn(s;&half);
  let b=task.spawn(s;&n32);
  let c=task.spawn(s;&u16v);
  let d=task.spawn(s;&nm);
  <task.awaitall(s)
};
TOKE

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=task:std.task;
i=math:std.math;
f=half():f64{<1.5};
f=n32():i32{<-5};
f=u16v():u16{<65000};
f=nm():str{<"hello"};
f=main():i64{
  let s=task.scope();
  let a=task.spawn(s;&half);
  let b=task.spawn(s;&n32);
  let c=task.spawn(s;&u16v);
  let d=task.spawn(s;&nm);
  let e=task.spawn(s;fn():f64{<0.25});
  let rc=task.awaitall(s);
  let nz=if(task.result(d)!=0){"str-ok"}el{"str-null"};
  io.println("\(math.frombits(task.result(a))) \(task.result(b)) \(task.result(c)) \(nz) \(math.frombits(task.result(e)))");
  <0
};
TOKE

check_ir() {   # $1 = .ll
  python3 - "$1" <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
bad = []
calls = re.findall(r"call i64 @tk_task_spawn_fv\(([^)]*)\)", ir)
if len(calls) != 4:
    bad.append("expected 4 tk_task_spawn_fv calls, found %d" % len(calls))
want = {"half": ("double", r"bitcast double %c\w+ to i64"),
        "n32": ("i32", r"sext i32 %c\w+ to i64"),
        "u16v": ("i16", r"zext i16 %c\w+ to i64"),
        "nm": ("i64", r"ret i64 %c\w+")}
seen = set()
for cargs in calls:
    a = [x.strip().split()[-1] for x in cargs.split(",")]
    if len(a) != 3: bad.append("call with %d args" % len(a)); continue
    m = re.search(re.escape(a[1]) + r" = ptrtoint i64 \(i64\)\* @(__tk_fvc_\w+) to i64", ir)
    f = re.search(re.escape(a[2]) + r" = ptrtoint [^@]*@p_(\w+) to i64", ir)
    if not m or not f: bad.append("call args are not (adapter, &fn)"); continue
    fn = f.group(1); seen.add(fn)
    d = re.search(r"^define ([^\n]*?) @" + m.group(1) + r"\(i64 %fv\)[^{\n]*\{(.*?)^\}", ir, re.M | re.S)
    if not d: bad.append("adapter for %s not defined (i64 %%fv)" % fn); continue
    head, body = d.groups()
    if "fastcc" in head: bad.append("adapter for %s is fastcc" % fn)
    rty, widen = want[fn]
    if not re.search(r"call fastcc %s %%\w+\(\)" % re.escape(rty), body):
        bad.append("adapter for %s does not call the value fastcc %s" % (fn, rty))
    if len(re.findall(widen, body)) < 2:
        bad.append("adapter for %s does not widen with %r on both branches" % (fn, widen))
if seen != set(want): bad.append("functions handed over: %s" % sorted(seen))
print("\n".join(bad) if bad else "OK")
PY
}

if "${TKC}" --emit-llvm --out host.ll ir.tk >host.err 2>&1; then
  r=$(check_ir host.ll); [ "$r" = OK ] && pass "host IR: f64 bitcast, i32 sext, u16 zext, str as i64" || fail "host IR: $r"
else fail "emit-llvm (host) failed: $(head -c 300 host.err)"; fi

if "${TKC}" --target x86_64-unknown-linux-gnu --emit-llvm --out x86.ll ir.tk >x86.err 2>&1; then
  r=$(check_ir x86.ll); [ "$r" = OK ] && pass "x86_64 IR: f64 bitcast, i32 sext, u16 zext, str as i64" || fail "x86_64 IR: $r"
else fail "emit-llvm (x86_64) failed: $(head -c 300 x86.err)"; fi

if "${TKC}" --allow-all --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  want="1.5 -5 65000 str-ok 0.25"
  [ "$rc" -eq 0 ] && [ "$out" = "$want" ] && pass "runs: $want" || fail "runs: rc=$rc out=$out (want $want)"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2306: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
