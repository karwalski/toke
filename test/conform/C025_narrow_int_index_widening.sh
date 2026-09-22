#!/usr/bin/env bash
# C025_narrow_int_index_widening.sh — an index of any integer width must be
# widened to i64 by its own type, not by a guess (story 137.3).
#
# The four collection-index coercion sites in llvm.c were each a two-way
# if/else:
#
#     if (strcmp(ity, "i64")) {
#         if (!strcmp(ity, "i8*"))  ptrtoint i8* ... to i64
#         else                      zext i1 ... to i64      <-- everything else
#     }
#
# So the ONLY non-i64 index the compiler could actually widen was a bool.
# Every other width fell into the `zext i1` default and clang rejected the
# module: "'%tN' defined with type 'i32' but expected 'i1'".  loke's report
# named `i32` and array indices; the reporter's own reading — "the zext i1
# looks like a default branch being taken for any non-i64 integer" — is
# correct, and the breadth is wider than the report: i8, i16, i32 and their
# unsigned spellings all fail, at every one of the four sites (array
# subscript, map `.get` on a map local, map `.get` on a struct field, and the
# module-qualified `alias.get`).
#
# The fix routes all four through coerce_value(), the one function that
# already knows every (src,dst) pair — the same "consult the operand's actual
# type" rule 127.27 applied to map-literal values and did not generalise.
#
# Cases 1-6: every narrow width, as an array index, must build and read right.
# Case 7:    a str-keyed map `.get` — the OTHER arm of the same if/else
#            (ptrtoint i8*).  A fix that routed everything through the integer
#            path would pass 1-6 and break every string-keyed map.
# Case 8:    a map held in a struct field, the third of the four sites.
# Case 9:    an f64 index is still refused — widening it silently would be a
#            worse bug than the one being fixed.
#
# Stories: 137.3, 127.27, 57.13.1

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

WORK="$(mktemp -d /tmp/tkc_c025_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C025: a narrow-int index is widened by its own type"
echo "----------------------------------------------------------------"

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

# reject_build NAME SOURCE — the compiler must refuse (no binary produced).
reject_build() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "bad.tk"
    local out rc=0
    rm -f bad_bin
    out="$("${TKC}" --allow-all --out bad_bin "bad.tk" 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && [ ! -x bad_bin ]; then
        echo "  PASS: ${name} refused"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected the compiler to refuse, got rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
    fi
}

# ── Cases 1-6: every narrow integer width as an array index ─────────────
# Each of these emitted `zext i<width>` -> `zext i1` before the fix and was
# rejected by clang with E9003.  Every success path ends in an explicit `<`
# so that 127.113 (a tail expression without `<` silently returning 0) can
# not be mistaken for this defect.
for W in i8 i16 i32 u8 u16 u32; do
  run_case "array index typed ${W}" "20" \
"m=main;
i=io:std.io;
i=s:std.str;
f=pick(a:@(i64);idx:${W}):i64{
  <a.get(idx)
};
f=main():i64{
  let arr=@(10;20;30);
  io.println(s.fromint(pick(arr;1 as ${W})));
  <0
};"
done

# ── Case 7: the i8* arm of the same four sites must not regress ────────
# A str-keyed map .get is the OTHER branch of the if/else being replaced
# (ptrtoint i8* -> i64).  A fix that routed everything through the integer
# path would pass 1-6 and break every string-keyed map in the language.
#
# Note on the two cases NOT written here: a bool array index and a narrow
# map key are both refused by the type checker (E4031, "array index must be
# integer" / "map key must be 'i64'"), so the i1 arm and a narrow key are
# unreachable at these sites from source.  The i1 arm is preserved inside
# coerce_value regardless; it is simply not assertable from a .tk program.
run_case "str-keyed map .get still widens its key" "77" \
'm=main;
i=io:std.io;
i=s:std.str;
f=look(mm:@(str:i64);k:str):i64{
  <mm.get(k)
};
f=main():i64{
  let mm=@("a":77;"b":88);
  io.println(s.fromint(look(mm;"a")));
  <0
};'

# ── Case 8: a map held in a struct field — the third of the four sites ──
run_case "map in a struct field, str key" "77" \
'm=main;
i=io:std.io;
i=s:std.str;
t=Box{mm:@(str:i64);n:i64};
f=main():i64{
  let b=Box{mm:@("a":77;"b":88);n:1};
  io.println(s.fromint(b.mm.get("a")));
  <0
};'

# ── Case 9: an f64 index is refused, not silently widened ───────────────
# coerce_value knows a double->i64 pair (a bitcast, for the array ABI), which
# would be the wrong thing to do to an index.  The type checker must keep
# rejecting this shape; if it ever stops, this case says so loudly rather
# than letting a bit-pattern become a subscript.
reject_build "f64 array index" \
'm=main;
i=io:std.io;
i=s:std.str;
f=pick(a:@(i64);idx:f64):i64{
  <a.get(idx)
};
f=main():i64{
  let arr=@(10;20;30);
  io.println(s.fromint(pick(arr;1.0)));
  <0
};'

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
