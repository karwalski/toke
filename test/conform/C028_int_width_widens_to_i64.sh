#!/usr/bin/env bash
# C028_int_width_widens_to_i64.sh — toke widens implicitly to i64, in one
# direction only, and the map literal is typed (stories 127.114, 127.53).
#
# TWO CHANGES, ONE DECISION.
#
# (a) `bind_init_type()` in src/types.c had no NODE_MAP_LIT case, so
#     `let m=@("a":1)` — the commonest way to make a map — left the binding
#     TY_UNKNOWN.  Every map rule in the type checker was therefore dead code
#     for it: 127.12's `.len`/`.keys` properties, 127.106's E4035, and the
#     map-key type check NODE_INDEX_EXPR has carried since 113.B.12 had never
#     once fired for a plain map literal.  C017's own reject_check() says so
#     in a comment; that comment is updated by this change.
#
# (b) Switching (a) on is not free, and that is why the two-line fix was
#     written, verified, and then deliberately reverted: `m.len` becomes a u64
#     and a bool-valued `m.get` becomes a bool at call sites that declare i64,
#     so test_127_8 and test_127_27 — both correct programs — are rejected.
#     The same wall stopped 127.53 from typing `.len` globally: it dropped
#     test/standalone/run_all.sh to 378/22.
#
# OWNER DECISION 2026-09-22: implicit widening to i64, not a corpus migration
# sweep.  i64 is toke's one arithmetic integer, and every narrower or unsigned
# integer already occupies a full i64 slot at the ABI — a map value, an array
# element, the `.len` header word.  The coercion is a no-op in codegen.  A
# migration would have paid a large one-time cost to preserve a distinction
# the language does not otherwise surface.
#
# The direction is the whole of the rule, so most of this file is about the
# direction.  u64 -> i64 is accepted because the destination can hold every
# value toke can produce for it.  i64 -> u64 is NOT, and must stay an error:
# that one turns a negative into a huge positive in silence.  A symmetric
# "these are both integers, near enough" rule would pass every widening case
# below and still be wrong; cases 20-24 are the ones that tell them apart.
# Cases 25-27 guard the boundary the widening must not eat: int <-> float has
# no implicit promotion (ADR-0004, story 127.64).
#
# Stories: 127.114, 127.53, 127.52, 127.12, 127.106, 127.40, 127.64, 113.B.12

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_intwidth_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C028: integer widths widen to i64, one way, and a map literal is typed"
echo "----------------------------------------------------------------------"

# accept NAME EXPECTED_STDOUT SOURCE — must compile AND print the right thing.
#
# Asserted on stdout, not just on exit 0, because the claim is that the
# widening is a no-op: an accepted program that prints a different number
# would mean the coercion moved bits, which is a worse outcome than the
# E4031 it replaced.  Run at every -O level for the same reason.
accept() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "ok.tk"
    local o bad=0
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f ok_bin
        if ! "${TKC}" --allow-all "${o}" --out "ok_bin" "ok.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log | head -3
            FAIL=$((FAIL + 1)); bad=1
            continue
        fi
        out="$(./ok_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected '${expected}', got '${out}' (rc ${rc})"
            FAIL=$((FAIL + 1)); bad=1
        fi
    done
    [ "${bad}" -eq 0 ] && echo "  PASS: ${name} (4 -O levels)"
    return 0
}

# reject NAME CODE SOURCE — `tkc --check` must refuse, with that code.
reject() {
    local name="$1" code="$2" src="$3"
    printf '%s\n' "${src}" > "chk.tk"
    local out rc=0
    out="$("${TKC}" --check "chk.tk" 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q "${code}"; then
        echo "  PASS: ${name} rejected with ${code}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected --check to reject with ${code}, got rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
    fi
}

# ── Cases 1-6: u64 -> i64 widening, one per value-flow site ──────────────
# Each site is a separate `types_equal` call in src/types.c and each had to be
# converted on its own, so each is asserted on its own.  Before the change
# every one of these was E4031 "expected 'i64', got 'u64'".
#
# The array receivers are PARAMETERS (`a:@i64`), not `let a=@(1;2;3)`, and
# that is deliberate rather than stylistic.  `bind_init_type()` has no
# NODE_ARRAY_LIT case either — the array sibling of the map hole this story
# fixes — so a binding initialised from an array literal is still TY_UNKNOWN
# and `a.len` on it is untyped.  Written that way these cases would compile
# whether or not the widening rule exists, and would assert nothing at all.
# A typed receiver is what makes `.len` actually u64 at the check site.

echo "-- u64 -> i64, by value-flow site --"

# 1. comparison.  The `i<arr.len` loop condition: the single commonest shape
#    in the corpus, and the reason 127.53 existed at all.
accept "comparison i64 < u64" "3" \
'm=main;
i=io:std.io;
f=count(a:@i64):i64{
  let n=mut.0;
  lp(let i=0;i<a.len;i=i+1){n=n+1};
  <n
};
f=main():i64{
  i=io:std.io;
  io.println("\(count(@(10;20;30)))");
  <0
};'

# 2. arithmetic.  A-SRT-0090v38, the frozen corpus original, in shape: an i64
#    multiplied by a u64-typed length expression, then used as an index.
accept "arithmetic i64 * u64" "30" \
'm=main;
i=io:std.io;
f=pick(a:@i64;p:i64):i64{
  let idx=p*(a.len-1)/100;
  <a.get(idx)
};
f=main():i64{
  io.println("\(pick(@(10;20;30);100))");
  <0
};'

# 3. call argument.  This is the site test_127_8 trips: `ck(...;m.len;2)`
#    against a `got:i64` parameter.
accept "call argument u64 into i64 param" "3" \
'm=main;
i=io:std.io;
f=id(n:i64):i64{<n};
f=hand(a:@i64):i64{<id(a.len)};
f=main():i64{
  io.println("\(hand(@(10;20;30)))");
  <0
};'

# 4. let annotation.
accept "let n:i64 = u64" "3" \
'm=main;
i=io:std.io;
f=hand(a:@i64):i64{
  let n:i64=a.len;
  <n
};
f=main():i64{
  io.println("\(hand(@(10;20;30)))");
  <0
};'

# 5. return, against a declared i64 return type.
accept "return u64 from an i64 function" "3" \
'm=main;
i=io:std.io;
f=count(a:@i64):i64{<a.len};
f=main():i64{
  io.println("\(count(@(10;20;30)))");
  <0
};'

# 6. assignment to an existing i64 binding.
accept "assign u64 to an i64 binding" "3" \
'm=main;
i=io:std.io;
f=hand(a:@i64):i64{
  let n=mut.0;
  n=a.len;
  <n
};
f=main():i64{
  io.println("\(hand(@(10;20;30)))");
  <0
};'

# ── Cases 7-10: the receivers `.len` answers ─────────────────────────────
# 127.53 is "type `.len` globally".  Arrays and maps were already u64 in
# NODE_FIELD_EXPR; str was held back precisely because of the migration cost,
# and only check_std_str_args knew about it.  All three are typed now, so all
# three are asserted — a str whose `.len` is still untyped would pass cases
# 1-6 and silently leave half the story undone.

echo "-- .len is u64 on every receiver that answers it --"

accept "str .len widens at a call site" "6" \
'm=main;
i=io:std.io;
f=id(n:i64):i64{<n};
f=hand(s:str):i64{<id(s.len)};
f=main():i64{
  io.println("\(hand("abcdef"))");
  <0
};'

accept "str .len in a comparison" "6" \
'm=main;
i=io:std.io;
f=count(s:str):i64{
  let n=mut.0;
  lp(let i=0;i<s.len;i=i+1){n=n+1};
  <n
};
f=main():i64{
  io.println("\(count("abcdef"))");
  <0
};'

accept "str .len returned as i64" "6" \
'm=main;
i=io:std.io;
f=plen(x:str):i64{<x.len};
f=main():i64{
  io.println("\(plen("abcdef"))");
  <0
};'

accept "map .len widens at a call site" "2" \
'm=main;
i=io:std.io;
f=id(n:i64):i64{<n};
f=main():i64{
  let m=@("a":1;"b":2);
  io.println("\(id(m.len))");
  <0
};'

# ── Cases 11-13: bool -> i64 ─────────────────────────────────────────────
# The other half of what the NODE_MAP_LIT case surfaces.  `@("a":true)` has
# value type bool, so `m.get("a")` is a bool — at an i64 call site, which is
# how test_127_27 asserts it (`ck("bool-lit-true";m.get("a");1)`).  Same slot
# argument as u64: 127.27 coerces a bool map value to the i64 slot at the
# literal (i1 zext), so reading it back at i64 moves no bits.

echo "-- bool -> i64 (a bool in an i64 slot, read back) --"

accept "bool map value at an i64 call site" "1 0" \
'm=main;
i=io:std.io;
f=id(n:i64):i64{<n};
f=main():i64{
  let m=@("a":true;"b":false);
  io.println("\(id(m.get("a"))) \(id(m.get("b")))");
  <0
};'

accept "bool map value into an i64 annotation" "1" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":true);
  let n:i64=m.get("a");
  io.println("\(n)");
  <0
};'

accept "bool from an expression-valued map entry" "1 0" \
'm=main;
i=io:std.io;
f=id(n:i64):i64{<n};
f=main():i64{
  let m=@("x":1<2;"y":3>4);
  io.println("\(id(m.get("x"))) \(id(m.get("y")))");
  <0
};'

# ── Cases 14-19: the map literal is now typed at all ─────────────────────
# These are the rules that had never fired for `let m=@(...)`.  They are the
# point of landing the NODE_MAP_LIT case: without them the case would be a
# pure liability — new coercions and no new diagnostics.  Every one is
# asserted at --check, because being diagnosed a stage later than ideal is
# exactly what C017 had to settle for before this change.

echo "-- the rules that were dead code for a map literal --"

reject "wrong map key type on a map literal" "E4031" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1;"b":2);io.println("\(m.get(1))");<0};'

reject "wrong map key type on a mut. map literal" "E4031" \
'm=main;
i=io:std.io;
f=main():i64{let m=mut.@("a":1);io.println("\(m.get(1))");<0};'

reject "str key against an int-keyed map literal" "E4031" \
'm=main;
i=io:std.io;
f=main():i64{let m=@(1:10;2:20);io.println("\(m.get("a"))");<0};'

reject "a map property that does not exist (127.106, at --check)" "E4035" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.size)");<0};'

reject "a misspelt map property (127.106, at --check)" "E4035" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.kyes)");<0};'

# `.len` and `.keys` are the two real ones and must keep working, or the
# diagnostic above is a language regression wearing a fix's clothes.
accept "the two real map properties still work" "3 a" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1;"b":2);
  let k=m.keys;
  io.println("\(m.len + 1) \(k.get(0))");
  <0
};'

# ── Cases 20-24: the direction.  These are the load-bearing rejects ──────
# Everything above would also pass under a symmetric "both are integers"
# rule.  Nothing below would.  i64 -> u64 can turn a negative into a huge
# positive with no cast written and no diagnostic, which is the failure mode
# the decision is specifically not buying.  (This is not hypothetical: the
# first cut of types_compat here WAS symmetric, and every case above passed.)

echo "-- i64 -> u64 is NOT implicit (the direction is the rule) --"

reject "let n:u64 = i64" "E4031" \
'm=main;
i=io:std.io;
f=main():i64{let k:i64=2;let n:u64=k;io.println("\(n)");<0};'

reject "i64 argument into a u64 parameter" "E4031" \
'm=main;
i=io:std.io;
f=takes(n:u64):i64{<0};
f=main():i64{let k:i64=2;io.println("\(takes(k))");<0};'

reject "return i64 from a u64 function" "E4031" \
'm=main;
f=bad(k:i64):u64{<k};
f=main():i64{<0};'

reject "assign i64 to a u64 binding" "E4031" \
'm=main;
i=io:std.io;
f=main():i64{
  let n=mut.0 as u64;
  let k:i64=2;
  n=k;
  io.println("\(n)");
  <0
};'

# A negative is the value that makes the direction matter, so name it.
reject "a negative i64 does not become a u64 in silence" "E4031" \
'm=main;
i=io:std.io;
f=takes(n:u64):i64{<0};
f=main():i64{let k:i64=0-1;io.println("\(takes(k))");<0};'

# ── Cases 25-27: the boundary the widening must not eat ──────────────────
# ADR-0004 / story 127.64: toke has no implicit int <-> float promotion, and
# 127.64's whole point is that `bytes/1048576.0` must stay an error rather
# than silently becoming integer division.  "Integer width" is not
# "numeric", and a widening rule written one predicate too wide would take
# f64 with it.

echo "-- int <-> float stays an error (ADR-0004, 127.64) --"

reject "u64 length divided by a float literal" "E4031" \
'm=main;
i=io:std.io;
f=hand(a:@i64):i64{io.println("\(a.len/2.0)");<0};
f=main():i64{<hand(@(1;2))};'

reject "let x:f64 = u64" "E4031" \
'm=main;
i=io:std.io;
f=hand(a:@i64):i64{let x:f64=a.len;io.println("\(x)");<0};
f=main():i64{<hand(@(1;2))};'

reject "let n:i64 = f64" "E4031" \
'm=main;
i=io:std.io;
f=main():i64{let x:f64=1.5;let n:i64=x;io.println("\(n)");<0};'

reject "f64 argument into an i64 parameter" "E4031" \
'm=main;
i=io:std.io;
f=takes(n:i64):i64{<0};
f=main():i64{let x:f64=1.5;io.println("\(takes(x))");<0};'

echo "----------------------------------------------------------------------"
echo "C028: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ] || exit 1
exit 0
