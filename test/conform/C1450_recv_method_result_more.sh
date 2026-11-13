#!/usr/bin/env bash
# C1450_recv_method_result_more.sh — the rest of the receiver methods whose
# result kind the lowering establishes are typed, and a `mut` binding of such
# a result is typed when every store to it agrees (story 127.371).
#
# THE DEFECT.  127.364 (C1390) typed array .slice and the str-to-str set
#   (slice/concat/trim/upper/lower/replace/substr), through an immutable `let`
#   only.  Still unjudged: `a.sort(&cmp).concat(b)` on arrays checked clean,
#   as did `s.split(",").upper()` and `let x=mut.a.slice(0;2); x.concat(b)`
#   (which printed 2: an array has no .concat).
#
# THE FIX.  recv_result_on (types.c) types the result by what the lowering
#   returns (llvm.c receiver dispatch and expr_struct_type tags; std.str
#   `split(..):@$str`):
#     array .sort/.filter -> the receiver's @T; array .join -> str
#     str .sub/.substring/.join -> str; str .split/.chars/.fields -> @str
#   and recv_method_mut_result_type types a `mut` binding of a receiver-method
#   result when EVERY assignment to that binding stores the identical type (a
#   preserving method on itself, another such result, or a str literal for a
#   str) -- the 127.315 model.  Any other store leaves it untyped, as before.
#
# CASES.
#   1. `a.sort(&cmp).concat(b)` is E4036 (got array.concat).
#   2. `a.filter(&odd).upper()` is E4036 (got array.upper).
#   3. `s.split(",").trim()` / `.chars().lower()` / `.fields().upper()` are
#      E4036 (got array.*).
#   4. `s.sub(..).keys()` / `s.substring(..).push(1)` / `p.join(",").keys()`
#      are E4036 (got str.*).
#   5. `let x=mut.a.slice(0;2); x.concat(b)` is E4036, fix (x+b) (D390).
#   6. a mut binding re-stored by preserving methods / a str literal is typed:
#      `s=s.upper(); s="z"; s.keys()` is E4036.
#   7. a mut binding re-stored with anything else stays untyped: clean.
#   8. valid programs over every new member build and print as on the base.
#
# Negative control: fix/z161-mt-iface-partial (6dca2454) passes cases 1-6
# with --check rc 0 (measured 2026-11-13).
#
# Story: 127.371
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1450_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C1450: more receiver-method results typed; mut bindings of them (127.371)"
echo "---------------------------------------------------------------------------"

# expect_gots <name> <file> <got>... — --check fails with exactly one E4036
# per <got>, carrying those "got" values in order
expect_gots() {
    local name=$1 file=$2; shift 2
    "${TKC}" --check "${file}" > "${file}.out" 2>&1; local rc=$?
    local have want
    have=$(grep '"error_code":"E4036"' "${file}.out" \
           | grep -o '"got":"[^"]*"' | sed 's/"got":"//;s/"$//' | paste -s -d' ' -)
    want="$*"
    if [ "${rc}" -ne 0 ] && [ "${have}" = "${want}" ]; then pass "${name}"
    else fail "${name}: rc=${rc}, got [${have}], want [${want}]: $(head -c 400 "${file}.out")"; fi
}

# ── 1. sort ──────────────────────────────────────────────────────────────
cat > c1.tk <<'TOKE'
m=t;
f=cmp(p:i64;q:i64):i64{<p-q};
f=main():i64{
  let a=@(3;1;2);
  let b=@(9);
  let d=a.sort(&cmp).concat(b);
  <0
};
TOKE
expect_gots "a.sort(&cmp).concat(b) is E4036" c1.tk "array.concat"
grep -q '"fix"' c1.tk.out && fail "a call receiver got a fix: $(cat c1.tk.out)" \
    || pass "no fix for a call receiver"

# ── 2. filter ────────────────────────────────────────────────────────────
cat > c2.tk <<'TOKE'
m=t;
f=odd(p:i64):bool{<p%2==1};
f=main():i64{
  let a=@(3;1;2);
  let d=a.filter(&odd).upper();
  <0
};
TOKE
expect_gots "a.filter(&odd).upper() is E4036" c2.tk "array.upper"

# ── 3. split / chars / fields -> @str ────────────────────────────────────
cat > c3.tk <<'TOKE'
m=t;
f=main():i64{
  let s="a,b c";
  let p=s.split(",").trim();
  let q=s.chars().lower();
  let r=s.fields().upper();
  <0
};
TOKE
expect_gots "split/chars/fields results are arrays" c3.tk "array.trim" "array.lower" "array.upper"

# ── 4. sub / substring / join -> str ─────────────────────────────────────
cat > c4.tk <<'TOKE'
m=t;
f=main():i64{
  let s="hello";
  let p=@("x";"y");
  let a=s.sub("l";"L").keys();
  let b=s.substring(0;2).push(1);
  let c=p.join(",").keys();
  let d=",".join(p).push(2);
  <0
};
TOKE
expect_gots "sub/substring/join results are str" c4.tk "str.keys" "str.push" "str.keys" "str.push"

# ── 5. the story's mut form ──────────────────────────────────────────────
cat > c5.tk <<'TOKE'
m=t;i=io:std.io;
f=main():i64{
  let a=@(1;2;3);
  let b=@(9);
  let x=mut.a.slice(0;2);
  let c=x.concat(b);
  io.println("\(c.len)");
  <0
};
TOKE
expect_gots "let x=mut.a.slice(0;2); x.concat(b) is E4036" c5.tk "array.concat"
grep -q '"fix":"(x+b)"' c5.tk.out && pass "the mut form gets fix (x+b)" \
    || fail "missing fix (x+b): $(head -c 400 c5.tk.out)"

# ── 6. agreeing stores keep the mut binding typed ────────────────────────
cat > c6.tk <<'TOKE'
m=t;
f=cmp(p:i64;q:i64):i64{<p-q};
f=main():i64{
  let a=@(3;1;2);
  let b=@(9);
  let x=mut.a.slice(0;2);
  x=x.slice(0;1);
  lp(let i=0;i<2;i=i+1){ x=a.sort(&cmp) };
  let c=x.concat(b);
  let s=mut."a,b".trim();
  s=s.upper();
  s="z";
  let k=s.keys();
  <0
};
TOKE
expect_gots "agreeing stores keep a mut binding typed" c6.tk "array.concat" "str.keys"

# ── 7. any other store leaves it untyped ─────────────────────────────────
cat > c7.tk <<'TOKE'
m=t;
f=main():i64{
  let a=@(1;2;3);
  let b=@(9);
  let x=mut.a.slice(0;2);
  x=x.append(4);
  let c=x.concat(b);
  let s=mut.@("x";"y").join(",");
  s=s.split(",");
  let t=s.concat("q");
  <0
};
TOKE
if "${TKC}" --check c7.tk > c7.out 2>&1; then pass "a disagreeing store leaves the binding untyped"
else fail "a disagreeing store was judged: $(head -c 400 c7.out)"; fi

# ── 8. valid programs are unchanged ──────────────────────────────────────
cat > c8.tk <<'TOKE'
m=t;i=io:std.io;
f=cmp(p:i64;q:i64):i64{<p-q};
f=odd(p:i64):bool{<p%2==1};
f=main():i64{
  let a=@(5;3;4;1);
  let s=a.sort(&cmp).slice(0;3);
  let o=a.filter(&odd).sort(&cmp);
  let w="a b  c".fields();
  let ch="xyz".chars();
  let p="a,b,c".split(",");
  let j=p.join("-").upper();
  let u="hello".sub("l";"L").substring(0;4);
  let g=",".join(p).concat("!");
  let m=mut.a.sort(&cmp);
  m=m.slice(1;3);
  let q=mut."  t  ".trim();
  q=q.concat("x");
  io.println("\(s.len) \(s.get(0)) \(o.len) \(o.get(1)) \(w.len) \(w.get(2)) \(ch.len) \(ch.get(1)) \(p.len) \(j) \(u) \(g) \(m.len) \(m.get(0)) \(q)");
  <0
};
TOKE
if "${TKC}" --out c8 c8.tk > c8.log 2>&1; then
    out="$(./c8)"; want='3 1 3 3 3 c 3 y 3 A-B-C heLL a,b,c! 2 3 tx'
    [ "${out}" = "${want}" ] && pass "valid programs build and print as before" \
        || fail "valid programs printed '${out}', want '${want}'"
else
    fail "valid programs rejected: $(head -c 400 c8.log)"
fi

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
