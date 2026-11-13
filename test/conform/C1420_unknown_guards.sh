#!/usr/bin/env bash
# C1420_unknown_guards.sh — the two "TY_UNKNOWN anywhere" guards that asked
# tk_type_identical(t,t) (story 127.374).
#
# THE DEFECT.  tk_type_identical returns 1 on pointer equality before it
# looks inside a type, so tk_type_identical(t,t) is 1 for `@(?)` and
# `@(rec)` with an unknown field.  Two guards relied on that self-compare:
#
#   std_array_mut_ret (127.315): "its initialiser is a std [T] with no
#     unknown element".  The guard never fired: `let a=mut.cl.recs()`, a
#     user interface returning `[rec]` whose `rec` has a field the checker
#     cannot type, was judged anyway (section A, case 1).  It now asks
#     tk_type_has_unknown and declines, as 127.315 specified; the immutable
#     binding (no such guard) and a fully typed mut binding are judged as
#     before (cases 2-3).  The untyped field is `k:$u`, a field of a SUM
#     type: imported_struct_type declines a sum, so the importer leaves it
#     TY_UNKNOWN.  (Until 127.376 the fixture was `cb:(i64):i64`, which the
#     generated .tki spelled `(`; it is now spelled `(i64):i64`, and an
#     importer that learns to read that spelling would type the field.)
#
#   known_value_type (127.326 / 127.327): read "no TY_UNKNOWN anywhere" but
#     always admitted a partial type.  Refusing it (tk_type_has_unknown) was
#     tried and REGRESSES valid code: the mt binding below lost its map type
#     and `d.get("k")` lowered as an array read and trapped RT003.  So the
#     rule is now stated as what it did -- top-level known, not void -- and
#     section B pins the partial-type adoption (cases 4-7) next to the
#     fully typed and fully unknown controls (cases 8-9).  Two arms of
#     distinct partial types disagreed (tk_type_identical) and left the
#     binding untyped, filed as 127.375; case 10 now pins that they agree
#     when their shape and unknown positions match (arm_types_agree, C1432).
#
# Negative control: TKC built from integrate/z-wave-1113b (c1f6408b; case 1
# re-checked with the `k:$u` fixture against a pre-127.374 build, 2026-11-13) --
# case 1 red (E4031: the guard never fired); cases 2-9 green on both; case 10
# (as updated by 127.375) red on any build without arm_types_agree (rc=0).
#
# Story: 127.374

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
RUN="${REPO_ROOT}/test/run_test.sh"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# codes <out> -- "<line>:<code>" for each diagnostic, space-separated
codes() {
    printf '%s\n' "$1" | grep -o '"error_code":"[A-Z0-9]*".*"line":[0-9]*' \
      | sed -E 's/"error_code":"([A-Z0-9]*)".*"line":([0-9]*)/\2:\1/' | tr '\n' ' '
}

# ── A: std_array_mut_ret (127.315) ────────────────────────────────────────
mkdir -p "${WORK}/mm"
cat > "${WORK}/mm/a_lib.tk" <<'EOF'
m=c1420.lib;
t=$u{$a:i64;$b:i64};
t=$rec{n:i64;k:$u};
f=recs():@$rec{ <@($rec{n:1;k:$u{$a:1}}) };
EOF
cat > "${WORK}/mm/b_main.tk" <<'EOF'
m=c1420.main;
i=s:std.str;i=io:std.io;
i=cl:c1420.lib;
f=main():i64{
  let a=mut.cl.recs();
  io.println(s.fromint(s.len(a) as i64));
  let b=cl.recs();
  io.println(s.fromint(s.len(b) as i64));
  let c=mut.s.split("a,b";",");
  io.println(s.fromint(s.len(c) as i64));
  <0
};
EOF
( cd "${WORK}/mm" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
[ -f "${WORK}/mm/c1420.lib.tki" ] || echo "  FAIL: setup: --emit-interface produced no c1420.lib.tki"
outA="$(cd "${WORK}/mm" && "${TKC}" --check b_main.tk 2>&1)"
cA=" $(codes "${outA}")"
expect "1 mut [rec] with an untyped field: not judged (127.315 guard)" \
  "$(printf '%s' "${cA}" | grep -c ' 6:E4031 ')" "0"
expect "2 immutable [rec]: judged as before" \
  "$(printf '%s' "${cA}" | grep -c ' 8:E4031 ')" "1"
expect "3 mut [str]: judged as before (C901)" \
  "$(printf '%s' "${cA}" | grep -c ' 10:E4031 ')" "1"

# ── B: known_value_type (127.326 / 127.327) ───────────────────────────────
H='m=mb;i=s:std.str;i=io:std.io;t=$er{m:str};f=g(x:str):i64{<s.len(x) as i64};f=r(n:i64):i64!$er{<n};'
chk() {
    printf '%s\nf=main():i64{\nlet c=true;\n%s\n<0};\n' "$H" "$1" > "${WORK}/cb.tk"
    local out rc g
    out="$("${TKC}" --check "${WORK}/cb.tk" 2>&1)"; rc=$?
    g="$(printf '%s' "$out" | grep -o '"error_code":"[A-Z0-9]*"\|"got":"[^"]*"' | head -2 | cut -d'"' -f4 | tr '\n' ' ')"
    echo "rc=${rc} ${g:-none}"
}
run() {
    printf '%s\nf=main():i64{\nlet c=true;\n%s\n<0};\n' "$H" "$1" > "${WORK}/rb.tk"
    "${RUN}" 60 "${TKC}" "${WORK}/rb.tk" -o "${WORK}/rb" >/dev/null 2>&1 || { echo build-failed; return; }
    "${RUN}" 20 "${WORK}/rb" 2>&1 | tr '\n' ' '
}
MK='let d=mt r(1) {$ok:v @("k":s.bytes("ab")); $err:e <1};'
MA='let d=mt r(1) {$ok:v @(s.bytes("ab")); $err:e <1};'
expect "4 mt arm @(str:?): d is a map, d.get(k) reads the [byte]" \
  "$(run "${MK} let e=d.get(\"k\"); io.println(\"\\(e.get(1))\");")" "98 "
expect "5 mt arm @(?): d.len" \
  "$(run "${MA} io.println(\"\\(d.len)\");")" "1 "
expect "6 mt arm @(?) into a str parameter" \
  "$(chk "${MA} io.println(\"\\(g(d))\");")" "rc=1 E4031 @(?) "
expect "7 mt arm @(?) into an i64 let" \
  "$(chk "${MA} let y:i64=d;")" "rc=1 E4031 @(?) "
expect "8 control: fully typed arm" \
  "$(chk 'let d=mt r(1) {$ok:v @(1;2); $err:e <1}; let y:i64=d;')" "rc=1 E4031 @(i64) "
expect "9 control: an unknown arm leaves d untyped" \
  "$(chk 'let d=mt r(1) {$ok:v s.bytes("ab"); $err:e <1}; let y:i64=d;')" "rc=0 none"
expect "10 two partial arms of one shape agree (127.375, C1432)" \
  "$(chk 'let d=if(c){@(s.bytes("ab"))}el{@(s.bytes("cd"))}; let y:i64=d;')" "rc=1 E4031 @(?) "

echo "C1420: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
