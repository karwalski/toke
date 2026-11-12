#!/usr/bin/env bash
# C1330_struct_literal_field_types.sh — a struct literal's field values are
# judged against the declared field types (story 127.356).
#
# `t=$r{a:i64;b:f64}; let r=$r{a:"x";b:3}` passed --check and printed a
# pointer and a denormal.  Each field value is now held to the rule an
# annotated `let x:T=v` uses (types_compat, or an integer literal that fits):
# E4031 with expected/got, the position of the field VALUE, and no `fix`.
# An integer literal into an f64 field is rejected exactly as `let b:f64=3`
# is; a fitting literal into u8 is accepted exactly as `let b:u8=7` is.
#
# Negative control: TKC=<main's toke> -- every E4031 case fails (rc=0).
#
# Story: 127.356

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|expected|got|col|fix-or-ABSENT of each error joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    (cd "${WORK}" && "${TKC}" --check p.tk) > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("severity") != "error": continue
    out.append("%s|%s|%s|%s|%s" % (d.get("error_code"), d.get("expected"), d.get("got"),
                                d.get("pos", {}).get("col"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

# 1. The 127.356 repro: both fields are judged, each at its value's column.
expect "1 repro: str into i64, int literal into f64" \
    "$(diags 'm=a;i=io:std.io;t=$r{a:i64;b:f64};f=main():i64{let r=$r{a:"x";b:3};io.println("\(r.a) \(r.b)");<0};')" \
    "E4031|i64|str|59|ABSENT / E4031|f64|i64|65|ABSENT"
# 2. The `let` rule it mirrors: `let b:f64=3` is E4031 too.
expect "2 let b:f64=3 is the same E4031" \
    "$(diags 'm=a;f=main():i64{let b:f64=3;<0};')" "E4031|f64|i64|18|ABSENT"
# 3. Control: matching values, and a fitting integer literal into u8 (as at `let`).
expect "3 matching scalars and a fitting u8 literal stay clean" \
    "$(diags 'm=a;t=$r{a:i64;b:f64;c:u8;d:$str;e:bool};f=main():i64{let r=$r{a:1;b:3.0;c:7;d:"s";e:true};<r.a};')" "rc=0"
# 4. A literal that does not fit u8 is rejected, as `let z:u8=300` is.
expect "4 an unfitting u8 literal" \
    "$(diags 'm=a;t=$r{c:u8};f=main():i64{let r=$r{c:300};<0};')" "E4031|u8|i64|40|ABSENT"
# 5. Nested struct field: a scalar where a struct is declared; control nested literal.
expect "5 scalar into a nested struct field" \
    "$(diags 'm=a;t=$p{x:i64};t=$q{p:$p;n:i64};f=main():i64{let q=$q{p:5;n:1};<q.n};')" "E4031|p|i64|58|ABSENT"
expect "6 nested struct literal stays clean" \
    "$(diags 'm=a;t=$p{x:i64};t=$q{p:$p;n:i64};f=main():i64{let q=$q{p:$p{x:2};n:1};<q.p.x};')" "rc=0"
expect "7 wrong struct into a struct field" \
    "$(diags 'm=a;t=$p{x:i64};t=$o{x:i64};t=$q{p:$p};f=main():i64{let q=$q{p:$o{x:2}};<0};')" "E4031|p|o|65|ABSENT"
# 8. Array- and map-typed fields; controls with empty/typed-empty/narrowing literals.
expect "8 array field with the wrong element type" \
    "$(diags 'm=a;t=$r{xs:@(i64)};f=main():i64{let r=$r{xs:@("a")};<0};')" "E4031|@(i64)|@(str)|46|ABSENT"
expect "9 map field with the wrong value type" \
    "$(diags 'm=a;t=$r{m:@($str:i64)};f=main():i64{let r=$r{m:@("k":"v")};<0};')" "E4031|@(str:i64)|@(str:str)|49|ABSENT"
expect "10 array/map field controls stay clean" \
    "$(diags 'm=a;t=$r{xs:@(i64);m:@($str:i64);ys:@(u8);zs:@(f64)};f=main():i64{let r=$r{xs:@();m:@("k":1);ys:@(1;2);zs:@($f64)};<0};')" "rc=0"
# 11. Sum-typed field; control constructs the variant.
expect "11 scalar into a sum-typed field" \
    "$(diags 'm=a;t=$sh{$circ:f64;$sq:i64};t=$r{s:$sh;n:i64};f=main():i64{let r=$r{s:3;n:1};<r.n};')" "E4031|sh|i64|72|ABSENT"
expect "12 a variant into a sum-typed field stays clean" \
    "$(diags 'm=a;t=$sh{$circ:f64;$sq:i64};t=$r{s:$sh;n:i64};f=main():i64{let r=$r{s:$sh{$sq:2};n:1};<r.n};')" "rc=0"
# 13. `$err` box: a variant payload of the wrong type; controls incl. a void payload.
expect "13 \$err box payload of the wrong type" \
    "$(diags 'm=a;t=$err{$bad:$str;$code:i64};f=g(n:i64):i64!$err{if(n<0){<$err{$bad:5}};<n};f=main():i64{<0};')" "E4031|str|i64|72|ABSENT"
expect "14 \$err box payloads of the declared type, and a void variant, stay clean" \
    "$(diags 'm=a;t=$err{$bad:$str;$code:i64;$none:void};f=g(n:i64):i64!$err{if(n<0){<$err{$bad:"neg"}};if(n>9){<$err{$code:7}};if(n==5){<$err{$none:0}};<n};f=main():i64{<0};')" "rc=0"
# 15. A fallible function value in an infallible function field (127.276) is
#     still reported once, not twice.
expect "15 fallible fn value into an infallible fn field: one E4031" \
    "$(diags 'm=a;t=$e{$bad:i64};t=$h{f:(i64):i64};f=inner(x:i64):i64!$e{<x};f=main():i64{let h=$h{f:inner};<0};' | grep -o 'E4031' | wc -l | tr -d ' ')" "1"

# 16. Runtime control: a correctly typed literal reads back its values.
cat > "${WORK}/run.tk" <<'EOF'
m=a;
i=io:std.io;
t=$r{a:i64;b:f64;c:u8};
f=main():i64{let r=$r{a:7;b:2.5;c:9};io.println("\(r.a) \(r.b) \(r.c)");<0};
EOF
if (cd "${WORK}" && "${TKC}" run.tk -o run >/dev/null 2>&1); then
    expect "16 runtime: a well-typed literal reads back" "$("${WORK}/run")" "7 2.5 9"
else
    echo "FAIL 16 runtime: build failed"; FAIL=$((FAIL + 1))
fi

echo "C1330: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
