#!/usr/bin/env bash
# C2063_task_scope_registry.sh — std.task answers a value that is not a scope
# it created as it answers the 0 scope (-1), and never reads through it
# (story 138.47, the T-67 class for an i64-ABI handle).
#
# THE DEFECT.  task.tki declares every parameter `i64`, so no checker rule can
# see a forged scope; task.c cast the integer to a tk_scope and locked its
# mutex.  task.awaitall(5) and task.cancel(5) checked clean and died with
# SIGSEGV (Z213 survey); task.spawn(5;&f) read it the same way.
# task.result(5) already answered -1 (task handles go through a table).
#
# THE FIX.  tk_task_scope registers every scope in the runtime's handle
# registry (tk_runtime.c, 138.45) and spawn/awaitall/cancel look the scope up
# first; anything else gets -1.
#
# WHAT IS ASSERTED: forged scopes (5, a real scope + 8, a task handle, a vec
# handle's value) get -1 from spawn, awaitall and cancel; a real scope still
# runs its tasks (results 42 42), and cancel on it returns 0.
#
# Negative control: TKC=<base ee27591c tree>/toke -- the forged program exits
# 139; the real-scope program passes.
#
# Story: 138.47
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2063_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2063: std.task refuses a scope it did not create (138.47)"
echo "----------------------------------------------------------------------"

run_case() {   # name expected
    local name="$1" want="$2"
    cat > "${name}.tk"
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build ($(grep -m1 error "${name}.log" | cut -c1-200))"; return
    fi
    local got rc
    got="$(./"${name}" 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "${name}: ${got}"
    else fail "${name}: rc=${rc} got '${got}', want '${want}'"; fi
}

run_case forged "5: -1 -1 -1
moved: -1 -1 -1
taskhandle: -1 -1 -1
vec: -1 -1 -1
real: rc 0 results 42 42 cancel 0" <<'EOF'
m=forged;
i=io:std.io;
i=task:std.task;
i=vec:std.vec;
f=work():i64{<42};
f=probe(label:$str;s:i64):i64{
  io.println("\(label): \(task.spawn(s;&work)) \(task.awaitall(s)) \(task.cancel(s))");
  <0
};
f=main():i64{
  probe("5";5);
  let s=task.scope();
  probe("moved";s+8);
  let a=task.spawn(s;&work);
  probe("taskhandle";a);
  let v=vec.new();
  probe("vec";v as i64);
  let b=task.spawn(s;&work);
  let rc=task.awaitall(s);
  io.println("real: rc \(rc) results \(task.result(a)) \(task.result(b)) cancel \(task.cancel(s))");
  <0
};
EOF

echo "----------------------------------------------------------------------"
echo "C2063: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
