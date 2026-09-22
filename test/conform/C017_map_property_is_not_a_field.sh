#!/usr/bin/env bash
# C017_map_property_is_not_a_field.sh — a field name a map does not have is a
# diagnostic, not a raw byte read (story 127.106).
#
# `m.values` type-checked clean, compiled clean, exited 0 and printed a
# pointer-shaped decimal.  A map has exactly two property spellings — `.len`
# and `.keys` — and both return early in llvm.c's NODE_FIELD_EXPR.  Every
# other name fell past them to the struct-field path with no layout
# established: `si` was NULL, `struct_field_index()` gave its 0 fallback, and
# the code loaded word 0 of the TkMapImpl as if it were a field.
#
# The struct-layout work (eb9c901, stories 127.86/127.89/136.28) added exactly
# the diagnostic this needed — "no struct in scope declares a field 'x', and
# the type of this value is not established" — and then EXEMPTED map receivers
# from it, on the reasoning that map properties "are lowered (or not)
# elsewhere".  That is true of `len` and `keys`.  Nothing lowers the rest, so
# the exemption switched the diagnostic off precisely where it was needed.
# The fix narrows the exemption to the two names it describes.
#
# 127.106 recorded this as a regression from "fails to build".  It is not.
# The 131.2 probe record at its own sha (67db04aa) has values_prop building
# fine — it was the sibling probe `m.values()` that failed to build, on an
# undefined symbol.  Ten clean builds at 67db04aa all exit 0 and print
# "34359738370 <pointer>", so the current behaviour is also the old behaviour.
# The read is undefined and its observable outcome is not stable across builds,
# which is why two independent `git bisect run` passes each blamed a docs-only
# commit.  Nothing introduced this; it was never diagnosed.
#
# Cases 1-7 are the defect, under every shape a map can arrive in — a fix that
# only reached the shapes the type checker can type would leave the commonest
# spelling of all, a plain map literal, still miscompiling.
# Cases 8-10 are the regression guard that matters most: the two real
# properties and the method forms must keep working, or this "fix" is a
# language regression dressed up as a diagnostic.
#
# Stories: 127.106, 127.86, 127.89, 136.28, 127.12, 127.41, 127.46

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

WORK="$(mktemp -d /tmp/tkc_mapprop_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C017: a map property the language does not have is a diagnostic"
echo "----------------------------------------------------------------"

# reject_build NAME SOURCE — the compiler must REFUSE, with E4035.
#
# Asserted against `--out`, not `--check`, because that is the guarantee this
# story is about: no shape may reach a runnable binary that prints an
# undefined read.  `--check` alone would under-assert — see reject_check.
reject_build() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "case.tk"
    local out rc=0
    rm -f case_bin
    out="$("${TKC}" --allow-all --out case_bin "case.tk" 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && [ ! -x case_bin ] && printf '%s' "${out}" | grep -q 'E4035'; then
        echo "  PASS: ${name} refused with E4035"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected the compiler to refuse with E4035, got rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -3
        [ -x case_bin ] && echo "      it produced a binary, which printed: $(./case_bin 2>&1)"
        FAIL=$((FAIL + 1))
    fi
}

# reject_check NAME SOURCE — `tkc --check` must ALSO reject.
#
# Only the shapes whose type the checker can recover are asserted here.
#
# 127.114 (2026-09-22) landed the NODE_MAP_LIT case in bind_init_type(), so
# two of the three shapes this comment used to excuse — a plain
# `let m=@("a":1)` and a `mut.@(...)` literal — are now diagnosed at --check
# and have been promoted below, as this comment said they should be the day
# NODE_MAP_LIT was typed.  (It could only land once the u64/bool -> i64
# widening rule existed: typing the binding makes `m.len` a u64 and a
# bool-valued `m.get` a bool, which rejected test_127_8 and test_127_27, both
# correct programs.  See C028.)
#
# The `.set`-derived map is still --check-clean and stays a reject_build:
# bind_init_type's NODE_CALL_EXPR case resolves user functions only, so a
# method call on a map is still TY_UNKNOWN and only the backend's 127.46 map
# tag knows `m2` is a map.  It is covered by reject_build above, so it does
# not miscompile; it is merely diagnosed one stage later than ideal.
reject_check() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "chk.tk"
    local out rc=0
    out="$("${TKC}" --check "chk.tk" 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q 'E4035'; then
        echo "  PASS: ${name} rejected by --check with E4035"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected --check to reject with E4035, got rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
    fi
}

# run_case NAME EXPECTED_STDOUT SOURCE — asserts stdout at -O0/-O1/-O2/-O3.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "ok.tk"
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f ok_bin
        if ! "${TKC}" --allow-all "${o}" --out "ok_bin" "ok.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log | head -3
            FAIL=$((FAIL + 1))
            continue
        fi
        out="$(./ok_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${o}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected '${expected}', got '${out}' (rc ${rc})"
            FAIL=$((FAIL + 1))
        fi
    done
}

# ── Case 1: the defect itself, as the 131.2 probe writes it ──────────────
# Before the fix: --check exit 0, build exit 0, run exit 0, stdout
# "34359738370 4340015260" — two undefined reads printed as decimals.
reject_build "m.values (the 127.106 probe)" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1;"b":2);
  let v=m.values;
  io.println("\(v.len) \(v.get(0))");
  <0
};'

# ── Case 2: every other plausible spelling is the same hole ─────────────
# Each printed a pointer at exit 0 before the fix.  Listed individually
# because a fix special-cased to the name `values` would pass case 1 and leave
# the defect standing.
reject_build "m.size" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.size)");<0};'
reject_build "m.count" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.count)");<0};'
reject_build "m.entries" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.entries)");<0};'
reject_build "m.items" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.items)");<0};'
reject_build "m.kyes (a typo)" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.kyes)");<0};'

# ── Case 3: an int-keyed map is the same rule ────────────────────────────
reject_build "int-keyed m.values" \
'm=main;
i=io:std.io;
f=main():i64{let m=@(1:10;2:20);io.println("\(m.values)");<0};'

# ── Case 4: a mut map literal ────────────────────────────────────────────
reject_build "mut.@() m.values" \
'm=main;
i=io:std.io;
f=main():i64{let m=mut.@("a":1);io.println("\(m.values)");<0};'

# ── Case 5: a map derived through .set ───────────────────────────────────
# The shape the checker still cannot type, even after 127.114: `bind_init_type`
# has no case for a method call on a map (its NODE_CALL_EXPR case resolves user
# functions only), so only the backend's 127.46 map tag knows m2 is a map.
# Without the llvm.c half of the fix this one keeps the defect.
reject_build "m2 = m.set(k;v) then m2.values" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1);
  let m2=m.set("b";2);
  io.println("\(m2.values)");
  <0
};'

# ── Case 6: a map parameter, and a map returned by a user function ──────
reject_build "a map parameter" \
'm=main;
i=io:std.io;
f=g(m:@(str:i64)):i64{io.println("\(m.values)");<0};
f=main():i64{<g(@("a":1))};'
reject_build "a map returned by a user fn" \
'm=main;
i=io:std.io;
f=mk():@(str:i64){<@("a":1)};
f=main():i64{let m=mk();io.println("\(m.values)");<0};'

# ── Case 7: a map held in a struct field ────────────────────────────────
reject_build "a map in a struct field" \
'm=main;
i=io:std.io;
t=Box{m:@(str:i64);n:i64};
f=main():i64{let b=Box{m:@("a":1);n:1};io.println("\(b.m.values)");<0};'

# ── The shapes the type checker can also catch, at --check ──────────────
# Pinned separately so that the day NODE_MAP_LIT is typed, the remaining three
# shapes can be promoted here rather than discovered by accident.
# Promoted from reject_build by 127.114 (see the note on reject_check above).
reject_check "a plain map literal" \
'm=main;
i=io:std.io;
f=main():i64{let m=@("a":1);io.println("\(m.values)");<0};'
reject_check "a mut.@() map literal" \
'm=main;
i=io:std.io;
f=main():i64{let m=mut.@("a":1);io.println("\(m.values)");<0};'
reject_check "annotated map binding" \
'm=main;
i=io:std.io;
f=main():i64{let m:@(str:i64)=@("a":1);io.println("\(m.values)");<0};'
reject_check "a map parameter" \
'm=main;
i=io:std.io;
f=g(m:@(str:i64)):i64{io.println("\(m.values)");<0};
f=main():i64{<g(@("a":1))};'
reject_check "a map returned by a user fn" \
'm=main;
i=io:std.io;
f=mk():@(str:i64){<@("a":1)};
f=main():i64{let m=mk();io.println("\(m.values)");<0};'
reject_check "a map in a struct field" \
'm=main;
i=io:std.io;
t=Box{m:@(str:i64);n:i64};
f=main():i64{let b=Box{m:@("a":1);n:1};io.println("\(b.m.values)");<0};'

# ── Case 8: `.len` and `.keys` — the two real properties — still work ────
# The regression guard.  A fix that diagnosed every map field name would pass
# every case above and delete two documented language features (127.12,
# 127.41).
run_case "m.len and m.keys still work" "2 a b" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1;"b":2);
  let k=m.keys;
  io.println("\(m.len) \(k.get(0)) \(k.get(1))");
  <0
};'

# ── Case 9: the method forms still work ──────────────────────────────────
# `m.get`/`m.set`/`m.contains`/`m.keys()`/`m.getor` all parse as a call whose
# callee is a NODE_FIELD_EXPR.  If the new diagnostic fired on the callee they
# would all break, so pin them.  Also pins 127.31 (contains) and 127.55 (set
# leaves the receiver alone: `m.contains("c")` is 0).
run_case "map methods still work" "1 2 1 0 2 9 0" \
'm=main;
i=io:std.io;
f=main():i64{
  let m=@("a":1;"b":2);
  let m2=m.set("c";3);
  let k=m.keys();
  io.println("\(m.get("a")) \(m2.get("b")) \(m.contains("a")) \(m.contains("z")) \(k.len) \(m.getor("zz";9)) \(m.contains("c"))");
  <0
};'

# ── Case 10: a user struct that really has a `values` field is untouched ─
# The diagnostic is gated on a map receiver.  A struct declaring the same
# field name must still resolve, or the fix has broken struct field access.
run_case "a struct field named values still resolves" "7" \
'm=main;
i=io:std.io;
t=Bag{values:i64;n:i64};
f=main():i64{
  let b=Bag{values:7;n:1};
  io.println("\(b.values)");
  <0
};'

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
