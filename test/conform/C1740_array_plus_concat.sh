#!/usr/bin/env bash
# C1740_array_plus_concat.sh — `+` on two arrays is concatenation, as
# semantics.md §2.4 and toke-spec-v0.4.md §J now state (story 127.359; owner
# decision 2026-11-12 §1.6: array `+` is the one spelling).
#
# This test pins the behaviour the spec text describes, so that the text and
# the compiler cannot drift apart silently:
#   - `@T + @T` checks clean and yields a NEW array, the left operand's
#     elements then the right's; neither operand changes;
#   - the element types must be equal (an element type that is not yet known,
#     as in `@()`, matches any); an integer-literal array operand whose values
#     all fit the other side's integer element type narrows to it (127.230);
#   - mismatched element types, and an array with a non-array operand, are
#     E4031 (`got` names the right operand's type);
#   - `-`, `*`, `/` on arrays and `+` on maps are E4031 ("requires numeric
#     operands");
#   - there is no `+=`: `a+=@(1)` is a parse error, E2002.
#
# The `fix` text these E4031s carry is NOT pinned here: it is prose rather
# than a mechanical repair, and story 127.436 is open against it.
#
# Story: 127.359

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
# code|got of the FIRST diagnostic; "rc=N" when there is none
diag1() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    print("%s|%s" % (d.get("error_code"), d.get("got"))); break
else:
    print("rc=" + sys.argv[2])
PY
}

expect "1 @i64 + @i64 literal" \
    "$(diag1 'm=q;f=main():i64{let a=@(1;2)+@(3);<0};')" "rc=0"
expect "2 typed parameters, result returned as @T" \
    "$(diag1 'm=q;f=g(x:@$str;y:@$str):@$str{<x+y};f=main():i64{<0};')" "rc=0"
expect "3 the empty literal matches any element type" \
    "$(diag1 'm=q;f=g(x:@i64):@i64{<@()+x+@()};f=main():i64{<0};')" "rc=0"
expect "4 nested arrays: @@i64 + @(@i64)" \
    "$(diag1 'm=q;f=g(x:@@i64;y:@i64):@@i64{<x+@(y)};f=main():i64{<0};')" "rc=0"
expect "5 integer literals narrow to a @u8 operand" \
    "$(diag1 'm=q;f=g(b:@u8):@u8{<@(1;2)+b+@(255)};f=main():i64{<0};')" "rc=0"
expect "6 mismatched element types: E4031" \
    "$(diag1 'm=q;f=main():i64{let a=@(1;2)+@("x");<0};')" "E4031|@(str)"
expect "7 int literal that does not fit u8: E4031" \
    "$(diag1 'm=q;f=g(b:@u8):@u8{<b+@(1;300)};f=main():i64{<0};')" "E4031|@(i64)"
expect "8 no int-to-float narrowing: @f64 + @(1) is E4031" \
    "$(diag1 'm=q;f=main():i64{let a=@(1.5)+@(1);<0};')" "E4031|@(i64)"
expect "9 nested element mismatch: @@i64 + @i64 is E4031" \
    "$(diag1 'm=q;f=g(x:@@i64;y:@i64):@@i64{<x+y};f=main():i64{<0};')" "E4031|@(i64)"
expect "10 array + scalar: E4031" \
    "$(diag1 'm=q;f=main():i64{let a=@(1;2)+3;<0};')" "E4031|i64"
expect "11 scalar + array: E4031" \
    "$(diag1 'm=q;f=main():i64{let a=3+@(1;2);<0};')" "E4031|@(i64)"
expect "12 '-' on arrays: E4031" \
    "$(diag1 'm=q;f=main():i64{let a=@(1;2)-@(1);<0};')" "E4031|@(i64)"
expect "13 '*' on arrays: E4031" \
    "$(diag1 'm=q;f=main():i64{let a=@(1;2)*@(1);<0};')" "E4031|@(i64)"
expect "14 '+' on maps: E4031" \
    "$(diag1 'm=q;f=main():i64{let a=@("a":1)+@("b":2);<0};')" "E4031|@(str:i64)"
expect "15 no '+=': parse error E2002" \
    "$(diag1 'm=q;f=main():i64{let a=mut.@(1);a+=@(2);<0};')" "E2002|="

# The accepted forms print what they built: order, length, operands untouched,
# the self-update accumulator and a @$str concat.
cat > "${WORK}/r.tk" <<'TK'
m=q;i=io:std.io;
f=main():i64{
  let a=@(1;2);
  let b=a+@(3);
  io.println("\(a.len) \(b.len) \(b.get(0)) \(b.get(1)) \(b.get(2))");
  let c=mut.@();
  lp(let k=0;k<4;k=k+1){c=c+@(k*10)};
  io.println("\(c.len) \(c.get(0)) \(c.get(3))");
  let x=@("p";"q")+@("r")+@();
  io.println("\(x.len) \(x.get(0))\(x.get(2))");
  let n=@(@(1);@(2))+@(@(3;4));
  io.println("\(n.len) \(n.get(2).len) \(n.get(2).get(1))");
  <0
};
TK
want="2 3 1 2 3 4 0 30 3 pr 3 2 4 "
if "${TKC}" "${WORK}/r.tk" -o "${WORK}/r.bin" >/dev/null 2>&1; then
    expect "16 runtime values" "$("${WORK}/r.bin" | tr '\n' ' ')" "${want}"
else
    expect "16 runtime values" "build failed" "${want}"
fi

echo "C1740: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
