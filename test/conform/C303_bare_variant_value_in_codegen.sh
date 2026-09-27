#!/usr/bin/env bash
# C303_bare_variant_value_in_codegen.sh — a bare sum-variant NAME in a value
# position that the checker let through stops the build with a located
# E4033, instead of lowering to a null box that traps when matched (story
# 138.13, loke T-13 field.tk).
#
# THE DEFECT. `let w=$w{l:$internal}` (a struct-literal field holding the
# variant name, not a value of it) passes --check; emit_expr had no case for
# NODE_TYPE_IDENT and its default lowered it to a silent `add i64 0, 0`, a
# null box, so `mt w.l {…}` dereferenced null and the program died SIGTRAP
# (exit 133) with toke exit 0.  llvm.c now emits E4033 "'$internal' is a
# type, not a value" at the name.  --check still accepts it: the checker
# half is reported separately (the `let l=mut.$internal` spelling is already
# E4033 there).
#
# Negative control: TKC built from main at c15dd11 — case 1 red (the build
# succeeds and the program exits 133).  Case 2 (the payload written out) is
# green on both.
#
# Story: 138.13 (T-13)

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

cat > "${WORK}/p1.tk" <<'EOF'
m=s13c;
t=$lvl{ $public:i64; $internal:i64 };
t=$w{ l:$lvl };
f=name(s:$lvl):i64{ <mt s { $public:v 0; $internal:v 1 } };
f=main():i64{
  let w=$w{l:$internal};
  <name(w.l)
};
EOF
rm -f "${WORK}/b"
out="$("${TKC}" "${WORK}/p1.tk" --out "${WORK}/b" 2>&1)"; rc=$?
if [ "${rc}" -ne 0 ]; then
    got="rc!=0 $(printf '%s' "${out}" | grep -o '"error_code":"E4033"' | head -1) $(printf '%s' "${out}" | grep -o '"line":6,"col":15' | head -1)"
else
    "${WORK}/b" >/dev/null 2>&1; got="built, exit $?"
fi
expect "1-bare-variant-field-is-E4033" "${got}" 'rc!=0 "error_code":"E4033" "line":6,"col":15'

cat > "${WORK}/p2.tk" <<'EOF'
m=s13c;
t=$lvl{ $public:i64; $internal:i64 };
t=$w{ l:$lvl };
f=name(s:$lvl):i64{ <mt s { $public:v 0; $internal:v 1 } };
f=main():i64{
  let w=$w{l:$lvl{$internal:0}};
  <name(w.l)
};
EOF
rm -f "${WORK}/b"
"${TKC}" "${WORK}/p2.tk" --out "${WORK}/b" >/dev/null 2>&1 && { "${WORK}/b"; echo "exit $?"; } > "${WORK}/o2" 2>&1 || echo build-failed > "${WORK}/o2"
expect "2-payload-written-out-control" "$(cat "${WORK}/o2")" "exit 1"

echo "C303: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
