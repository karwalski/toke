#!/usr/bin/env bash
# C006_interp_str_origins.sh — runtime conformance for string interpolation of
# `$str` values by ORIGIN (story 127.80).
#
# `"\(x)"` chooses its lowering from the compiler's belief about x's type.  A
# `$str` the compiler fails to recognise is handed to tk_str_fromi64_w and its
# POINTER is printed as a decimal — at exit 0, with no diagnostic.  Nothing
# fails, so nothing gets found; three live ooke log lines had been shipping it.
#
# The recognition used to come from two hand-maintained name lists: 26 of the
# 56 stdlib interface files were read into the return-type cache, and inside
# those, 25 wrapper symbols were spelled out in a `str_wrappers` table in
# expr_struct_type.  A string from anywhere else was untagged.  These cases pin
# one program per ORIGIN, run it at every optimisation level, and assert the
# printed text — a pointer-as-decimal fails the string compare loudly:
#
#   1. path.*            — a stdlib str return outside std.str (path.tki was
#                          never loaded: the module was absent from the list)
#   2. env.getor / fmt.* — the same, for modules whose str returns were never
#                          in str_wrappers
#   3. <module>.get(k)   — parses as a SUBSCRIPT, not a call, so it missed the
#                          stdlib-call branch entirely
#   4. map .get via `mt` — a match USED AS AN EXPRESSION dropped the tag
#   5. user `T!$err`     — the same, for a user error union opened with `mt`
#   6. control           — std.str / literals / numbers, which always worked
#                          and must keep working
#   7. loud failure      — an interpolation the compiler genuinely cannot type
#                          must be a diagnostic, never a plausible number; and
#                          the cases it CAN type must not be diagnosed
#
# Story: 127.80

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${REPO_ROOT}/tkc"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_interp_origin_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

# Case 3 reads this through env.get; fixed here so the expected text is fixed.
export TK_127_80_PROBE="/tk/127/80"

echo "C006: string interpolation keeps \$str strings from every origin"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE
#   Compiles SOURCE at -O0/-O1/-O2/-O3 and asserts stdout at each level.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "case.tk"
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f "case_bin"
        if ! "${TKC}" --allow-all "${o}" --out "case_bin" "case.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log
            FAIL=$((FAIL + 1))
            continue
        fi
        out="$(./case_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${o}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected '${expected}', got '${out}' (rc ${rc})"
            FAIL=$((FAIL + 1))
        fi
    done
}

# ── 1: a path-joining result — the story's headline case ────────────────────
#   std.path declares every one of these `"return": "str"` in stdlib/path.tki.
#   The file was on disk and unread, so all six printed their address.
run_case "path.* results interpolate as text" \
    "join=a/b dir=/x/y base=c.txt ext=.tk stem=c norm=a/b" '
m=t;
i=io:std.io;
i=p:std.path;
f=main():i64{
  let j=p.join("a";"b");
  io.println("join=\(j) dir=\(p.dir("/x/y/z.txt")) base=\(p.base("/x/y/c.txt")) ext=\(p.ext("c.tk")) stem=\(p.stem("/x/y/c.txt")) norm=\(p.normalize("a/./b"))");
  <0
};'

# ── 2: other stdlib modules whose str returns were never in str_wrappers ────
run_case "non-str stdlib modules interpolate as text" "miss=fallback b=true" '
m=t;
i=io:std.io;
i=e:std.env;
i=f:std.fmt;
f=main():i64{
  let d=e.getor("TK_127_80_DEFINITELY_UNSET";"fallback");
  io.println("miss=\(d) b=\(f.bool(true))");
  <0
};'

# ── 3: `<module>.get(k)` — the SUBSCRIPT spelling of a stdlib call ──────────
#   `env.get("HOME")` parses as NODE_INDEX_EXPR, not NODE_CALL_EXPR, so it
#   never reached the stdlib-call type branch at all.  PATH is set by the
#   harness so the expected text is fixed.
run_case "module .get(k) interpolates as text" "path=/tk/127/80" '
m=t;
i=io:std.io;
i=e:std.env;
f=main():i64{
  io.println("path=\(e.get("TK_127_80_PROBE"))");
  <0
};'

# ── 4: a map lookup opened with `mt` ────────────────────────────────────────
#   `mt` used as an EXPRESSION had a correct LLVM type and no struct tag, so
#   every string that reaches its use site through a match arrived untagged.
run_case "map .get through mt interpolates as text" "hit=vee miss=none both=vee/wye" '
m=t;
i=io:std.io;
f=main():i64{
  let m=@("k":"vee";"j":"wye");
  let hit=mt m.get("k"){$ok:v v;$err:e "none"};
  let miss=mt m.get("nope"){$ok:v v;$err:e "none"};
  let b2=mt m.get("j"){$ok:v v;$err:e "none"};
  io.println("hit=\(hit) miss=\(miss) both=\(hit)/\(b2)");
  <0
};'

# ── 5: a user error union over a string, opened with `mt` ───────────────────
run_case "user T!\$err string through mt interpolates as text" \
    "ok=okvalue err=recovered inline=okvalue" '
m=t;
i=io:std.io;
t=$err{$bad:$str};
f=risky(n:i64):$str!$err{
  if(n<0){<$err{$bad:"neg"}};
  <"okvalue"
};
f=main():i64{
  let a=mt risky(1){$ok:v v;$err:e "recovered"};
  let b=mt risky(0-1){$ok:v v;$err:e "recovered"};
  io.println("ok=\(a) err=\(b) inline=\(mt risky(1){$ok:v v;$err:e "recovered"})");
  <0
};'

# ── 6: CONTROL — the origins that already worked must keep working ──────────
#   std.str results, string literals, locals, ints, floats and bools all took
#   a correct path before 127.80 and must be untouched by the fix.  If a
#   number here ever prints as text (or a string as a number) the tag has
#   leaked across origins.  `\(true)` printing `1` (not `true`) is this
#   compiler's existing behaviour, pinned here as-is: 127.80 is about strings
#   that print as numbers, and changing a second thing in the same commit
#   would make a regression here ambiguous.
run_case "control: std.str, literals and numbers unchanged" \
    "cat=ab trim=x up=AB loc=held n=42 neg=-7 f=1.5 t=1 fi=99" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let held="held";
  io.println("cat=\(s.concat("a";"b")) trim=\(s.trim("  x  ")) up=\(s.upper("ab")) loc=\(held) n=\(42) neg=\(0-7) f=\(1.5) t=\(true) fi=\(s.fromint(99))");
  <0
};'

# ── 7: LOUD FAILURE — an untypeable interpolation must diagnose, not guess ──
#   Two layers now stand here and the case is pinned against BOTH, because
#   which one speaks first is an implementation detail: 136.1's member check
#   (E4027) rejects a call spelled `alias.method(...)` that neither the
#   interface nor the runtime declares, and 127.80's codegen backstop (E4032)
#   catches what reaches lowering still untyped — notably the `.get(k)`
#   SUBSCRIPT spelling, which the member check does not see.  Either code is a
#   pass; a clean compile is not.
loud_case() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "loud.tk"
    local out rc=0
    out="$("${TKC}" --allow-all -O0 --out "loud_bin" "loud.tk" 2>&1)" || rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -qE 'E4027|E4032'; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected a non-zero exit with E4027 or E4032, got rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

#   `s.index` does not exist (std.str declares `indexof`).  The compiler used
#   to invent tk_str_index_w, type it i64, and print whatever came back as a
#   decimal.
loud_case "undeclared stdlib method in an interpolation is refused, not guessed" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  io.println("v=\(s.index("hello";"l"))");
  <0
};'

#   The same, through the subscript spelling.  `path.get` does not exist; this
#   parses as an INDEX_EXPR, so only the codegen backstop sees it.
loud_case "undeclared member via the .get(k) subscript is refused, not guessed" '
m=t;
i=io:std.io;
i=p:std.path;
f=main():i64{
  io.println("v=\(p.get("x"))");
  <0
};'

#   COUNTERPART — the loud path must not eat correct programs.  `str.lastindexof`
#   is declared by the runtime glue and NOT by stdlib/str.tki (which declares
#   `lastindex`).  It links, it runs, and its i64 return makes the integer
#   default right.  An error here would be a false diagnostic on working code,
#   which is worse than the missing type: the interface gap is check-tki'"'"'s to
#   report, not this lowering'"'"'s.
run_case "runtime-declared, interface-silent method still compiles" "v=3" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  io.println("v=\(s.lastindexof("hello";"l"))");
  <0
};'

#   ...and a module with NO interface file at all (std.array is native glue)
#   must NOT be diagnosed: absence of a .tki is not evidence of anything, and
#   a false error here would break correct programs.
run_case "no-interface module (std.array) is not falsely diagnosed" "has=1 at=2" '
m=t;
i=io:std.io;
i=a:std.array;
f=main():i64{
  let xs=@(3;1;2);
  io.println("has=\(a.contains(xs;3)) at=\(a.indexof(xs;2))");
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
