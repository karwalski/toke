#!/usr/bin/env bash
# C2305_task_spawn_bool_c_adapter.sh — a bool function handed to task.spawn
# reaches the pool thread through a C-ABI adapter that returns a
# well-defined i64 (story 127.549).
#
# THE DEFECT (found by the 127.548 audit): `task.spawn(s;&pos)` with
# `f=pos():bool{..}` lowered to `tk_task_spawn(s, ptrtoint i1 ()* @..pos)`,
# and src/stdlib/task.c ran the value as `int64_t (*)(void)`.  pos is
# `define fastcc i1`, and x86-64 defines only bit 0 of the i1 return
# register, so task.result read undefined upper bits on linux/amd64 -- the
# hand-off 127.548 removed from the array combinators.  arm64 happens to
# clear the register, so this Mac reads the IR for the x86-64 half.
#
# THE FIX: task.spawn lowers to tk_task_spawn_fv(scope, adapter, value).  The
# adapter, `define internal i64 @__tk_fvc_<key>(i64 %fv)`, is C convention,
# calls the value as a toke call does (fastcc) and zero-extends the i1.
#
# WHAT THIS CHECKS (host IR, and IR for --target x86_64-unknown-linux-gnu):
#   1. tk_task_spawn_fv is declared (i64, i64, i64) and called once with 3
#      args; the bare tk_task_spawn is never called;
#   2. its 2nd argument is `ptrtoint i64 (i64)* @__tk_fvc_*`, and its 3rd the
#      function value `ptrtoint i1 ()* @p_pos`;
#   3. the adapter is not fastcc, returns i64, takes (i64 %fv), calls the
#      value `fastcc i1` and zexts the result on both of its branches;
#   4. the program runs on the host and prints `1 0` (task.result of a true
#      and of a false bool task).
#
# Negative control: main 58bd74e8 fails 1-3 on both targets (it calls
# tk_task_spawn with the bare value); its run passes 4 on arm64, which
# clears the register.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2305_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2305: a bool fn-ref handed to task.spawn goes through a C-ABI i64 adapter (127.549)"
echo "------------------------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=task:std.task;
f=pos():bool{<3>0};
f=neg():bool{<3<0};
f=main():i64{
  let s=task.scope();
  let a=task.spawn(s;&pos);
  let rc=task.awaitall(s);
  let t=task.scope();
  let b=task.spawn(t;&neg);
  let rd=task.awaitall(t);
  io.println("\(task.result(a)) \(task.result(b))");
  <0
};
TOKE

cat > ir.tk <<'TOKE'
m=p;
i=task:std.task;
f=pos():bool{<3>0};
f=main():i64{
  let s=task.scope();
  let a=task.spawn(s;&pos);
  let rc=task.awaitall(s);
  <task.result(a)
};
TOKE

check_ir() {   # $1 = .ll
  python3 - "$1" <<'PY'
import re, sys
ir = open(sys.argv[1]).read()
bad = []
if "declare i64 @tk_task_spawn_fv(i64, i64, i64)" not in ir:
    bad.append("tk_task_spawn_fv not declared (i64, i64, i64)")
if re.search(r"call i64 @tk_task_spawn\(", ir):
    bad.append("the bare tk_task_spawn is still called")
calls = re.findall(r"call i64 @tk_task_spawn_fv\(([^)]*)\)", ir)
if len(calls) != 1:
    bad.append("expected one tk_task_spawn_fv call, found %d" % len(calls))
else:
    args = [a.strip() for a in calls[0].split(",")]
    if len(args) != 3:
        bad.append("tk_task_spawn_fv called with %d args" % len(args))
    else:
        reg = args[1].split()[-1]
        m = re.search(re.escape(reg) + r" = ptrtoint (.*?) to i64", ir)
        if not m:
            bad.append("2nd argument %s is not a ptrtoint" % reg)
        elif not re.fullmatch(r"i64 \(i64\)\* @__tk_fvc_\w+", m.group(1)):
            bad.append("2nd argument is %r, not a C-ABI adapter" % m.group(1))
        else:
            sym = m.group(1).split("@")[1]
            d = re.search(r"^define ([^\n]*?) @" + sym + r"\(([^\n]*?)\)[^{\n]*\{(.*?)^\}", ir, re.M | re.S)
            if not d:
                bad.append("adapter %s not defined" % sym)
            else:
                head, params, body = d.groups()
                if "fastcc" in head: bad.append("adapter is fastcc: " + head)
                if not head.endswith("i64"): bad.append("adapter does not return i64: " + head)
                if params != "i64 %fv": bad.append("adapter params: " + params)
                if not re.search(r"call fastcc i1 %\w+\(\)", body):
                    bad.append("adapter does not call the value fastcc i1")
                if len(re.findall(r"zext i1 %\w+ to i64", body)) < 2:
                    bad.append("adapter does not zext the i1 on both branches")
        fv = args[2].split()[-1]
        if not re.search(re.escape(fv) + r" = ptrtoint i1 \(\)\* @p_pos to i64", ir):
            bad.append("3rd argument %s is not the function value &pos" % fv)
print("\n".join(bad) if bad else "OK")
PY
}

if "${TKC}" --emit-llvm --out host.ll ir.tk >host.err 2>&1; then
  r=$(check_ir host.ll); [ "$r" = OK ] && pass "host IR: task.spawn(&pos) gets a C-ABI zext adapter" || fail "host IR: $r"
else fail "emit-llvm (host) failed: $(head -c 300 host.err)"; fi

if "${TKC}" --target x86_64-unknown-linux-gnu --emit-llvm --out x86.ll ir.tk >x86.err 2>&1; then
  r=$(check_ir x86.ll); [ "$r" = OK ] && pass "x86_64 IR: task.spawn(&pos) gets a C-ABI zext adapter" || fail "x86_64 IR: $r"
else fail "emit-llvm (x86_64) failed: $(head -c 300 x86.err)"; fi

if "${TKC}" --allow-all --out p p.tk >build.err 2>&1; then
  out=$(./p 2>&1); rc=$?
  [ "$rc" -eq 0 ] && [ "$out" = "1 0" ] && pass "runs: task.result of true/false bool tasks is 1 0" || fail "runs: rc=$rc out=$out"
else fail "build failed: $(head -c 300 build.err)"; fi

echo ""
echo "C2305: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
