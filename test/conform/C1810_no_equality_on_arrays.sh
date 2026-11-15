#!/usr/bin/env bash
# C1810_no_equality_on_arrays.sh — `==` / `!=` with an array operand is
# refused (E4031), not miscompiled (story 138.20, loke T-60; owner decision
# 2026-11-15, register 1.2 (a)).
#
# The emitter has no array equality.  Against the literal `@()` it compared
# the array's address with a fresh allocation, so `a==@()` was false for every
# empty `a` (loke's `h==@()` spawn-failure checks never fired); between two
# named arrays it ran tk_str_cmp over the blocks, so `@(256)==@(512)` was true.
# The refusal sits beside 138.9's struct/sum refusal (C249).
#
# `fix` is the rewritten comparison, `x.len==0` / `x.len!=0`, and is given
# ONLY when one operand is the literal `@()` and the other is an identifier of
# array type (either order).  Two named arrays, a process handle, a str, a
# map, `@()` against `@()`, and an array operand that is not an identifier
# (a field, a call, an index) get none.
#
# `got` is the array the comparison is about: with exactly one operand the
# literal `@()` and the other an array, the other operand (`@()==x`, x of
# type @i64, gives '@(i64)'); otherwise the left operand if it is an array,
# else the right.
#
# Negative control: TKC=<v3.2.0 toke> -- cases 1-9 and 13-17 fail (rc=0, no
# diagnostic: the miscompiled comparison checked clean).  Cases 10-12 pass
# there too: a handle, str or map against `@()` was already E4031, as a type
# mismatch with no fix.  18-22 are unchanged by design.
#
# Story: 138.20

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
# code|got|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
# The fix must be a repair: apply it to the source and the program checks clean.
fixed() { printf '%s\n' "$1" > "${WORK}/p.tk"
    fix="$("${TKC}" --check "${WORK}/p.tk" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    if "fix" in d: print(d["fix"]); break')"
    [ -n "${fix}" ] || { echo "no fix"; return; }
    python3 - "${WORK}/p.tk" "$2" "${fix}" <<'PY'
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
assert s.count(old) == 1, "expected exactly one %r" % old
open(p, "w").write(s.replace(old, new))
PY
    "${TKC}" --check "${WORK}/p.tk" > /dev/null 2>&1; echo "rc=$?"
}

# --- refused, with the fix -------------------------------------------------
expect "1 typed array == @()" \
    "$(diags 'm=q;f=main():i64{let x:@str=@();if(x==@()){<1};<0};')" "E4031|@(str)|x.len==0"
expect "2 @() == typed array (either order)" \
    "$(diags 'm=q;f=g(x:@i64):bool{<@()==x};f=main():i64{<0};')" "E4031|@(i64)|x.len==0"
expect "3 array != @()" \
    "$(diags 'm=q;f=main():i64{let x=mut.@(1);if(x!=@()){<1};<0};')" "E4031|@(i64)|x.len!=0"
expect "4 @() != array" \
    "$(diags 'm=q;f=main():i64{let x:@i64=@(1);if(@()!=x){<1};<0};')" "E4031|@(i64)|x.len!=0"
expect "5 inside a larger condition" \
    "$(diags 'm=q;f=main():i64{let x:@i64=@(1);if(true&&x==@()){<1};<0};')" "E4031|@(i64)|x.len==0"
expect "6 the fix of case 1 checks clean" \
    "$(fixed 'm=q;f=main():i64{let x:@str=@();if(x==@()){<1};<0};' 'x==@()')" "rc=0"
expect "7 the fix of case 4 checks clean" \
    "$(fixed 'm=q;f=main():i64{let x:@i64=@(1);if(@()!=x){<1};<0};' '@()!=x')" "rc=0"

# --- refused, no fix -------------------------------------------------------
expect "8 two named arrays (T-60 contents)" \
    "$(diags 'm=q;f=main():i64{let p:@i64=@(1;2);let q:@i64=@(1;2;3);if(p==q){<1};<0};')" "E4031|@(i64)|ABSENT"
expect "9 two named arrays, !=" \
    "$(diags 'm=q;f=main():i64{let p=@(256);let q=@(512);if(p!=q){<1};<0};')" "E4031|@(i64)|ABSENT"
expect "10 process handle == @() (the loke spawn idiom)" \
    "$(diags 'm=q;i=process:std.process;f=main():i64{let h=mt process.spawn(@("/x")) {$ok:h h;$err:e <1};if(h==@()){<2};<0};')" "E4031|@(?)|ABSENT"
expect "11 str == @()" \
    "$(diags 'm=q;f=main():i64{let s="a";if(s==@()){<1};<0};')" "E4031|@(?)|ABSENT"
expect "12 map == @() (not an array type)" \
    "$(diags 'm=q;f=main():i64{let m=@("a":1);if(m==@()){<1};<0};')" "E4031|@(?)|ABSENT"
expect "13 @() == @()" \
    "$(diags 'm=q;f=main():i64{if(@()==@()){<1};<0};')" "E4031|@(?)|ABSENT"
expect "14 a field of array type == @() (not an identifier)" \
    "$(diags 'm=q;t=$pt{xs:@i64};f=main():i64{let p=$pt{xs:@(1)};if(p.xs==@()){<1};<0};')" "E4031|@(i64)|ABSENT"
expect "15 a call returning an array == @()" \
    "$(diags 'm=q;f=g():@i64{<@()};f=main():i64{if(g()==@()){<1};<0};')" "E4031|@(i64)|ABSENT"
expect "16 an index into @@i64 == @()" \
    "$(diags 'm=q;f=main():i64{let x:@@i64=@(@(1));if(x.get(0)==@()){<1};<0};')" "E4031|@(i64)|ABSENT"
expect "17 untyped empties (T-60 repro): x==y no fix, x==@() fix" \
    "$(diags 'm=q;f=main():i64{let x=@();let y=@();let u=x==y;let v=x==@();<0};')" \
    "E4031|@(?)|ABSENT / E4031|@(?)|x.len==0"

# --- unchanged -------------------------------------------------------------
expect "18 struct refusal unchanged (C249)" \
    "$(diags 'm=q;t=$pt{x:i64};f=main():i64{let a=$pt{x:1};let b=$pt{x:1};if(a==b){<1};<0};')" "E4031|pt|ABSENT"
expect "19 sum refusal unchanged (C249)" \
    "$(diags 'm=q;t=$lvl{$public:i64;$restricted:i64};f=main():i64{let a=$lvl{$restricted:0};let b=$lvl{$restricted:1};if(a!=b){<1};<0};')" "E4031|lvl|ABSENT"
expect "20 scalar and str == unchanged" \
    "$(diags 'm=q;f=main():i64{let a=1;let b=2;let s="x";if(a==b){<1};if(s!="y"){<2};if(1.5==2.5){<3};<0};')" "rc=0"
expect "21 .len and element comparisons stay clean" \
    "$(diags 'm=q;f=main():i64{let x:@i64=@(1;2);if(x.len==0){<1};if(x.get(0)!=1){<2};<0};')" "rc=0"

# A program that compiled before 138.20 is unaffected, and the rewrites the fix
# offers compute what `==`/`!=` against `@()` was meant to: 1+2+4+8 = 15.
cat > "${WORK}/r.tk" <<'TK'
m=q;i=io:std.io;
f=main():i64{
  let x:@str=@();
  let y=@();
  let z=mut.@(1);
  let r=mut.0;
  if(x.len==0){ r=r+1 }el{};
  if(y.len==0){ r=r+2 }el{};
  if(z.len!=0){ r=r+4 }el{};
  if(true&&x.len==0){ r=r+8 }el{};
  if(z.get(0)==1){ io.println("ok") }el{};
  <r
};
TK
if "${TKC}" "${WORK}/r.tk" -o "${WORK}/r.bin" >/dev/null 2>&1; then
    out="$("${WORK}/r.bin")"; rc=$?
    expect "22 unaffected program runs" "${out} rc=${rc}" "ok rc=15"
else
    expect "22 unaffected program runs" "build failed" "ok rc=15"
fi

echo "C1810: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
