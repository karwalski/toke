#!/usr/bin/env bash
# C931_mt_armbind_scope.sh — an `mt` arm binding ends with its arm
# (story 127.321).
#
# THE DEFECT (cf297baa).  The emitter predicts a match's result slot type from
# its arm bodies BEFORE it binds the arm's name.  A first-use binding (`y`,
# not shadowing anything, so not renamed `y.N`) kept its local-type entry
# after the arm, so a LATER match whose arm binds the same name predicted its
# body against the earlier binding's type: `{$ok:y y+1.0;..}` then
# `{$ok:y y+1;..}` stored the i64 6 into a double slot and printed
# 2.47033e-323 (silent, exit 0).  Renaming the second binding hid it.
#
# THE FIX.  llvm.c ptr_arm_release retires a first-use binding's local-type
# entry as scope_retire_name retires a block's `let` (127.196): renamed
# `name.~`, so lookups miss it and make_unique_name still sees the slot taken.
#
# Negative control: C931_TKC=<other tkc>.  At cf297baa cases 1-3 fail and
# 4 (the control) passes.
#
# Story: 127.321
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C931_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c931_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C931: an mt arm binding ends with its arm (127.321)"
echo "---------------------------------------------------------------------------"

# runs name description expected-output  (source on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    cat > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed"; fi
    if [ "${got}" = "${want}" ]; then echo "  PASS: ${desc} (${want})"; PASS=$((PASS + 1))
    else echo "  FAIL: ${desc}: expected '${want}', got '${got}'"; FAIL=$((FAIL + 1)); fi
}

runs a1 'a binding name reused across two map-lookup matches (f64 then i64)' '3.5 5 ' <<'EOF'
m=a1;i=io:std.io;
f=main():i64{
  let n=@("o":@("i":2.5));
  io.println("\(mt n.get("o").get("i"){$ok:y y+1.0;$err:e 0.0})");
  let p=@("o":@("i":4));
  io.println("\(mt p.get("o").get("i"){$ok:y y+1;$err:e 0})");
  <0
};
EOF
runs a2 'reused across user T!E calls (f64 payload, then i64)' 'c=3 d=4 ' <<'EOF'
m=a2;i=io:std.io;t=$e{code:i64};
f=g(x:i64):f64!$e{<x as f64/2.0};
f=h(x:i64):i64!$e{<x*1};
f=main():i64{
  io.println("c=\(mt g(3){$ok:w w*2.0;$err:e 0.0}) d=\(mt h(3){$ok:w w+1;$err:e 0})");
  <0
};
EOF
runs a3 'reused after an if-block match and by the err binding' 'x=2.5 y=7 z=-1 ' <<'EOF'
m=a3;i=io:std.io;t=$e{code:i64};
f=g(x:i64):f64!$e{<x as f64/2.0};
f=h(x:i64):i64!$e{<x+2};
f=main():i64{
  let k=5;
  if(k>0){io.println("x=\(mt g(k){$ok:v v+0.0;$err:e 0.0})")};
  io.println("y=\(mt h(k){$ok:v v*1;$err:e 0})");
  io.println("z=\(mt h(k){$ok:e 0-1;$err:v 0})");
  <0
};
EOF
runs a4 'control: a later let of the same name is its own slot' 'a=2.5 w=12 ' <<'EOF'
m=a4;i=io:std.io;t=$e{code:i64};
f=g(x:i64):f64!$e{<x as f64/2.0};
f=main():i64{
  io.println("a=\(mt g(5){$ok:w w;$err:e 0.0})");
  let w=11;
  io.println("w=\(w+1)");
  <0
};
EOF
echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
