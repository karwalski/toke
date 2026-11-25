#!/usr/bin/env bash
# C2309_task_spawn_operand_e4081.sh — what task.spawn / `spawn` runs must be
# a function value that takes no parameters: E4081 otherwise (story 127.549).
#
# THE DEFECT: task.tki declares task.spawn's second parameter `i64`, so the
# checker accepted anything integer-shaped and the runtime called it:
# `task.spawn(s;5)` jumped to address 5, a function with parameters read
# garbage for them, and `task.spawn(s;w())` / `sc{ let h=spawn w(); }`
# called w at once, on this thread, and spawned its RESULT as if it were
# code (127.549; the keyword form also fails to link without
# `i=task:std.task`, filed separately).
#
# THE FIX: E4081 (type_check, structured, no `fix` -- whether `&w`, a
# closure or another function was meant is the program's call) when the
# operand's type is known and is not a `():T` function value.
#
# WHAT THIS CHECKS (tkc --check):
#   1. E4081 for an i64, a function with a parameter, a call returning i64,
#      a closure with a parameter (task.spawn), and `spawn w()` / `spawn
#      &add` (keyword), each at the operand's line, with expected/got and no
#      fix field;
#   2. no diagnostic for `&w`, a bare function name, a closure, a call
#      returning a ():i64 closure, `spawn &w`, and a std.process `spawn`.
#
# Negative control: main 58bd74e8 fails 1 (all six pass --check).
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2309_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2309: task.spawn / spawn operand must be a ():T function value -- E4081 (127.549)"
echo "-----------------------------------------------------------------------------------"

cat > bad.tk <<'TOKE'
m=main;
i=task:std.task;
f=w():i64{<42};
f=add(a:i64):i64{<a};
f=main():i64{
  let s=task.scope();
  let a=task.spawn(s;5);
  let b=task.spawn(s;&add);
  let c=task.spawn(s;w());
  let e=task.spawn(s;fn(x:i64):i64{<x});
  sc{ let h=spawn w(); };
  sc{ let h2=spawn &add; };
  <0
};
TOKE

cat > good.tk <<'TOKE'
m=main;
i=task:std.task;
i=p:std.process;
f=w():i64{<42};
f=mk(k:i64):():i64{<fn():i64{<k}};
f=main():i64{
  let s=task.scope();
  let k=3;
  let a=task.spawn(s;&w);
  let b=task.spawn(s;w);
  let c=task.spawn(s;fn():bool{<k>2});
  let d=task.spawn(s;mk(4));
  sc{ let h=spawn &w; };
  let rc=task.awaitall(s);
  let q=p.spawn(@("true"));
  <0
};
TOKE

"${TKC}" --check bad.tk >bad.out 2>&1
r=$(python3 - bad.out <<'PY'
import json, sys
diags = []
for line in open(sys.argv[1]):
    line = line.strip()
    if not line.startswith("{"): continue
    try: diags.append(json.loads(line))
    except ValueError: pass
bad = []
e = [d for d in diags if d.get("error_code") == "E4081"]
lines = sorted(d["pos"]["line"] for d in e)
if lines != [7, 8, 9, 10, 11, 12]:
    bad.append("E4081 at lines %s, want [7..12]" % lines)
for d in e:
    if d.get("stage") != "type_check": bad.append("stage %r" % d.get("stage"))
    if d.get("severity") != "error": bad.append("severity %r" % d.get("severity"))
    if "fix" in d and d["fix"]: bad.append("line %d carries a fix" % d["pos"]["line"])
    if d.get("expected") != "():T": bad.append("line %d expected=%r" % (d["pos"]["line"], d.get("expected")))
got = {d["pos"]["line"]: d.get("got") for d in e}
for ln, want in ((7, "i64"), (8, "(i64):i64"), (9, "i64"), (12, "(i64):i64")):
    if got.get(ln) != want: bad.append("line %d got=%r, want %r" % (ln, got.get(ln), want))
others = [d.get("error_code") for d in diags if d.get("error_code") != "E4081" and d.get("severity") == "error"]
if others: bad.append("other errors: %s" % others)
print("\n".join(bad) if bad else "OK")
PY
)
[ "$r" = OK ] && pass "E4081 for i64, (i64):i64, a call, a 1-param closure, spawn w(), spawn &add" || fail "bad.tk: $r"

if "${TKC}" --check good.tk >good.out 2>&1 && ! grep -q '"severity":"error"' good.out; then
  pass "&w, bare w, closure, closure-returning call, spawn &w, process spawn: clean"
else fail "good.tk: $(head -c 400 good.out)"; fi

echo ""
echo "C2309: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
