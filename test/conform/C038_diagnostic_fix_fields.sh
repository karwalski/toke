#!/usr/bin/env bash
# C038_diagnostic_fix_fields.sh — the diagnostics themselves are the artefact.
#
# Stories: 127.140 (P0), 127.54, 127.136, 127.122, 127.135, 136.63.
#
# WHY THESE ARE ONE FILE.  toke's stated design is structured diagnostics with
# a machine-actionable `fix` field, read by an automated repair loop.  Every
# case below is that design failing in a way no existing gate could see: the
# compiler exited non-zero, so `make conform` was green, while the text it
# printed was wrong, self-contradictory, or absent.  AGENTS.md §2 ranks a wrong
# error message above a missing feature and §3.1 forbids an incorrect `fix`
# outright — so this file asserts the CODE, the POSITION and the FIX TEXT, not
# merely that something was rejected.  A test that only checked "exit 1" would
# have passed against every defect here.
#
# THE P0 (127.140).  `src/types.c`'s E4031 for `"a"+"b"` carried this `fix`:
#
#     "...for delimiter-joined collections use s.join(arr;sep)..."
#
# That is the REVERSED argument order.  `str.join` is declared (stdlib/str.tki)
# as `str.join(str; [str])` — the receiver is the SEPARATOR.  The reversed call
# the compiler recommended passes `--check`, builds, and exits 139:
#
#     s.join(arr;",")  -> --check 0, build 0, run 139   (the compiler's advice)
#     s.join(",";arr)  -> --check 0, build 0, prints "a,b"
#
# So a model or agent following the compiler's own instruction wrote a program
# that segfaulted, and the compiler accepted it.  Case A2 executes both orders
# rather than reading the string, because reading the string is exactly what
# failed to catch this for four months.
#
# Story 111.2 recorded that this case "produces a clear diagnostic".  It never
# did: the headline read `type mismatch: expected 'str', got 'str'` from
# f20bc55, the commit that introduced it, because emit_mm formats the two
# OPERAND types and for str+str they are the same type.  Case A1 pins that a
# diagnostic can never again name the same thing on both sides here.
#
# THE CONTROLS ARE HALF THE FILE.  It is easy to close a diagnostic gap by
# rejecting programs that are fine; 136.36 is on record as exactly that, and
# §2 says a false diagnostic on a correct program is worse than a missing one.
# Section F pins the surfaces these changes could plausibly have broken —
# std.vec handle receivers, `.tki`-imported record methods, the `tpl.*` / `df.*`
# / `row.*` sub-namespaces, a local binding that legally shadows a primitive
# type keyword, and the good diagnostics that already worked (E3011's
# "did you mean", E9004, E4027 under a non-colliding alias).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The tkc symlink is relinked by any concurrent `make`; $TKC lets a caller pin
# a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_diagfix_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C038: diagnostic code, position and fix text"
echo "--------------------------------------------------"

# ── helpers ────────────────────────────────────────────────────────────────

# emits NAME CODE LINE COL SOURCE — `--check` must report CODE at LINE:COL.
# Position is asserted because a confidently wrong location is what points the
# repair loop at the wrong line (the 127.116 harm).  Pass '-' for LINE or COL
# to skip that half.
emits() {
    local name="$1" code="$2" want_line="$3" want_col="$4" src="$5"
    printf '%s\n' "${src}" > "r.tk"
    local out rc=0
    out="$("${TKC}" --check "r.tk" 2>&1)" || rc=$?
    local rec
    rec="$(printf '%s\n' "${out}" | grep -a "\"error_code\":\"${code}\"" | head -1)"
    if [ -z "${rec}" ]; then
        echo "  FAIL: ${name}: expected ${code}, check rc ${rc}"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -2
        FAIL=$((FAIL + 1)); return
    fi
    local gl gc
    gl="$(printf '%s' "${rec}" | sed 's/.*"line":\([0-9]*\).*/\1/')"
    gc="$(printf '%s' "${rec}" | sed 's/.*"col":\([0-9]*\).*/\1/')"
    if [ "${want_line}" != "-" ] && [ "${gl}" != "${want_line}" ]; then
        echo "  FAIL: ${name}: ${code} at line ${gl}, expected ${want_line}"
        FAIL=$((FAIL + 1)); return
    fi
    if [ "${want_col}" != "-" ] && [ "${gc}" != "${want_col}" ]; then
        echo "  FAIL: ${name}: ${code} at col ${gc}, expected ${want_col}"
        FAIL=$((FAIL + 1)); return
    fi
    echo "  PASS: ${name} — ${code} at line ${gl}, col ${gc}"
    PASS=$((PASS + 1))
}

# fixsays NAME CODE SUBSTRING SOURCE — the `fix` field of CODE must contain
# SUBSTRING.  This is the field AGENTS.md §3.1 governs and the field a repair
# loop executes, so it is asserted literally.
fixsays() {
    local name="$1" code="$2" want="$3" src="$4"
    printf '%s\n' "${src}" > "r.tk"
    local out
    out="$("${TKC}" --check "r.tk" 2>&1)"
    local rec
    rec="$(printf '%s\n' "${out}" | grep -a "\"error_code\":\"${code}\"" | head -1)"
    if [ -z "${rec}" ]; then
        echo "  FAIL: ${name}: no ${code} emitted at all"
        FAIL=$((FAIL + 1)); return
    fi
    if printf '%s' "${rec}" | grep -qa -- "${want}"; then
        echo "  PASS: ${name} — fix contains: ${want}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: fix did not contain: ${want}"
        printf '%s' "${rec}" | sed 's/.*"fix":"/      fix was: /;s/","source_line.*//' | head -1
        FAIL=$((FAIL + 1))
    fi
}

# fixlacks NAME CODE SUBSTRING SOURCE — the `fix` of CODE must NOT contain
# SUBSTRING.  Used to pin that the segfaulting argument order is gone: a test
# that only asserted the correct order would still pass if both were printed.
fixlacks() {
    local name="$1" code="$2" bad="$3" src="$4"
    printf '%s\n' "${src}" > "r.tk"
    local out
    out="$("${TKC}" --check "r.tk" 2>&1)"
    local rec
    rec="$(printf '%s\n' "${out}" | grep -a "\"error_code\":\"${code}\"" | head -1)"
    if [ -z "${rec}" ]; then
        echo "  FAIL: ${name}: no ${code} emitted at all"
        FAIL=$((FAIL + 1)); return
    fi
    if printf '%s' "${rec}" | grep -qa -- "${bad}"; then
        echo "  FAIL: ${name}: fix still contains the retired text: ${bad}"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: ${name} — fix no longer contains: ${bad}"
        PASS=$((PASS + 1))
    fi
}

# notselfidentical NAME CODE SOURCE — the diagnostic's "expected" and "got"
# must differ.  This is 127.54's whole complaint: a type mismatch reported
# between a type and itself gives a repair loop nothing to act on.
notselfidentical() {
    local name="$1" code="$2" src="$3"
    printf '%s\n' "${src}" > "r.tk"
    local out
    out="$("${TKC}" --check "r.tk" 2>&1)"
    local rec
    rec="$(printf '%s\n' "${out}" | grep -a "\"error_code\":\"${code}\"" | head -1)"
    if [ -z "${rec}" ]; then
        echo "  FAIL: ${name}: no ${code} emitted at all"
        FAIL=$((FAIL + 1)); return
    fi
    local e g
    e="$(printf '%s' "${rec}" | sed 's/.*"expected":"\([^"]*\)".*/\1/')"
    g="$(printf '%s' "${rec}" | sed 's/.*"got":"\([^"]*\)".*/\1/')"
    if [ "${e}" = "${g}" ]; then
        echo "  FAIL: ${name}: expected and got are both '${e}'"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: ${name} — expected '${e}' vs got '${g}'"
        PASS=$((PASS + 1))
    fi
}

# accepts NAME SOURCE — `--check` must exit 0.  Used for the controls: these
# are correct programs and must stay correct.
accepts() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "a.tk"
    local out rc=0
    out="$("${TKC}" --check "a.tk" 2>&1)" || rc=$?
    if [ "${rc}" -eq 0 ]; then
        echo "  PASS: ${name} — accepted"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: a correct program was rejected"
        printf '%s\n' "${out}" | sed 's/^/      /' | head -2
        FAIL=$((FAIL + 1))
    fi
}

# runs NAME EXPECTED SOURCE — the program must build AND run AND print
# EXPECTED.  A value, not a compile: 127.140's defect was that the recommended
# program compiled perfectly and then died at run time.
runs() {
    local name="$1" want="$2" src="$3"
    printf '%s\n' "${src}" > "x.tk"
    rm -f x_bin
    local bout brc=0
    bout="$("${TKC}" --allow-all --out x_bin "x.tk" 2>&1)" || brc=$?
    if [ ! -x x_bin ]; then
        echo "  FAIL: ${name}: did not build (rc ${brc})"
        printf '%s\n' "${bout}" | sed 's/^/      /' | head -2
        FAIL=$((FAIL + 1)); return
    fi
    local got rrc=0
    got="$(./x_bin 2>&1)" || rrc=$?
    if [ "${rrc}" -ne 0 ]; then
        echo "  FAIL: ${name}: built, then exited ${rrc} (139 = SIGSEGV)"
        FAIL=$((FAIL + 1)); return
    fi
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: ${name} — printed '${got}'"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: printed '${got}', expected '${want}'"
        FAIL=$((FAIL + 1))
    fi
}

# ── A. 127.140 — the fix field that pointed at a segfault ──────────────────
echo ""
echo "A. 127.140 (P0): the E4031 for str+str"

A_SRC='m=t;
f=main():i64{
  let a="x"+"y";
  <0
};'

# A1 — the headline named 'str' on both sides for four months.
notselfidentical "A1 str+str is not a mismatch between str and str" E4031 "${A_SRC}"

# A2 — the retired text, and the one that replaces it.
fixlacks "A2 the segfaulting order s.join(arr;sep) is gone" E4031 's.join(arr;sep)' "${A_SRC}"
fixsays  "A3 the fix names the declared order s.join(sep;arr)" E4031 's.join(sep;arr)' "${A_SRC}"

# A4 — position still names the operator, not the statement or the file.
emits "A4 E4031 lands on line 3" E4031 3 - "${A_SRC}"

# A5/A6 — both orders.  This is the assertion that would have caught the
# defect; every string-reading gate in the tree missed it.  A6 asserted that
# the retired order builds and dies; since 127.201 it no longer builds —
# --check rejects the array in str.join's C-string slot — so A6 pins that.
runs "A5 the fix's order s.join(sep;arr) runs and joins" 'a,b' \
'm=t;
i=s:std.str;
i=io:std.io;
f=main():i64{
  let arr=@("a";"b");
  io.print(s.join(",";arr));
  <0
};'

emits "A6 the retired order s.join(arr;sep) is E4031 at check (127.201)" E4031 6 - \
'm=t;
i=s:std.str;
i=io:std.io;
f=main():i64{
  let arr=@("a";"b");
  io.print(s.join(arr;","));
  <0
};'

# A7-A9 — the other three patterns the `fix` recommends were each executed
# before being kept.  A `fix` field is only as good as its least-checked
# clause, and three of the four clauses here had never been run.
runs "A7 the fix's interpolation clause works" 'xy' \
'm=t;
i=io:std.io;
f=main():i64{ let a="x"; let b="y"; io.print("\(a)\(b)"); <0 };'

runs "A8 the fix's s.concat clause works" 'xy' \
'm=t;
i=s:std.str;
i=io:std.io;
f=main():i64{ io.print(s.concat("x";"y")); <0 };'

runs "A9 the fix's builder clause works" 'xy' \
'm=t;
i=s:std.str;
i=io:std.io;
f=main():i64{
  let b=s.builder();
  s.add(b;"x");
  s.add(b;"y");
  io.print(s.build(b));
  <0
};'

# ── B. 127.54 — a mismatch reported between a type and itself ──────────────
echo ""
echo "B. 127.54: composite types print their element type"

# The filed repro was `f=e():@($rec){ <@($rec) }`.  It used to read
# `expected 'array', got 'array'` because type_name() returned the bare kind
# for every TY_ARRAY; once rendered, it read `expected '@(rec)', got
# '@(void)'`, and this block concluded the element types "genuinely differ".
# They do not: `@($rec)` is a typed-EMPTY literal of element type $rec, and
# the program is correct.  The '@(void)' was the checker mistyping the
# literal (fixed in Batch E2, e2e4ac4 -- the same defect that broke
# test_f64_array.tk and ooke), and B2 was pinning the `fix` that mistake
# produced, "cast return value to @(rec) using 'as'" -- an instruction that
# does not compile, which AGENTS.md §6 forbids.  Rewritten 2026-09-24:
#
#   B0  the old repro is a correct program and is accepted;
#   B1  a GENUINE array-vs-array mismatch still names both element types;
#   B2  and its fix field is empty, because no `as` cast repairs it.
accepts "B0 a typed-empty @(\$rec) returned as @(\$rec) is accepted" \
'm=t;
t=$rec{a:i64};
f=e():@($rec){ <@($rec) };
f=main():i64{<0};'

B_SRC='m=t;
t=$rec{a:i64};
f=e():@($rec){ <@(1) };
f=main():i64{<0};'

notselfidentical "B1 array-vs-array names both element types" E4031 "${B_SRC}"
fixlacks "B2 an array return mismatch offers no cast fix" E4031 '"fix":' "${B_SRC}"

# B3 — a map mismatch must be readable for the same reason.
notselfidentical "B3 map-vs-map is not 'map' vs 'map'" E4031 \
'm=t;
f=want(m:@(str:i64)):i64{<0};
f=main():i64{ let m=@("a":"b"); <want(m) };'

# ── C. 127.136 — a parameter must shadow a module function ─────────────────
echo ""
echo "C. 127.136: a parameter shadows a top-level function of the same name"

# `page` is not an exotic parameter name for a document library.  The type
# checker looked up module scope FIRST and fell back to the parameters, which
# inverts lexical scoping, so `page` in the body resolved to the FUNCTION and
# a correct program failed with `expected 'func', got 'i64'`.
accepts "C1 f=page(page:i64) type-checks" \
'm=t;
f=page(page:i64):i64{
  <page+1
};
f=main():i64{ <page(1) };'

# C2 — and it computes with the PARAMETER, not something else.  A test that
# only checked "no diagnostic" would pass on a compiler that silently used the
# wrong binding.
runs "C2 the parameter is the value used" '42' \
'm=t;
i=io:std.io;
f=page(page:i64):i64{
  <page+1
};
f=main():i64{ io.print("\(page(41))"); <0 };'

# C3 — the same shape for a str parameter, the case that disproved the filed
# row'"'"'s "inverted message in the reverse direction" sub-claim.
accepts "C3 a str parameter shadowing a func is fine too" \
'm=t;
f=title(title:str):str{
  <title
};
f=main():i64{ let x=title("a"); <0 };'

# ── D. 127.122 — E4027 suggests a real member ──────────────────────────────
echo ""
echo "D. 127.122: E4027 names the member the author meant"

D_SRC='m=t;
i=str:std.str;
f=main():i64{
  let x=str.concta("a";"b");
  <0
};'

emits   "D1 E4027 still fires for a misspelled member" E4027 4 - "${D_SRC}"
fixsays "D2 and now names the candidate" E4027 "did you mean 'str.concat'?" "${D_SRC}"

# D3 — the guard.  A member that is not close to any real one gets no guess:
# AGENTS.md §3.1 forbids a `fix` that is a guess, and 131.78 is the case
# study of an "obvious" suggestion that was wrong.  Since 127.243 it gets no
# fix at all -- the old generic text was advice, not a rewrite.
fixlacks "D3 an unrelated member carries no fix" E4027 '"fix":' \
'm=t;
i=str:std.str;
f=main():i64{
  let x=str.zzzzqqqwwww("a");
  <0
};'

# ── E. 127.135 / 136.63 — two silent paths to the linker ───────────────────
echo ""
echo "E. 127.135 / 136.63: calls that used to pass --check and die at clang"

# E1/E2 — 127.135.  `str.concat` with std.str unimported passed --check AND
# --lint in silence, then failed as `use of undefined value '%str'` in a temp
# .ll file the author never saw.  The trigger was whether the member is REAL:
# a FAKE member of the same receiver already got a clean E9004.  So the one
# case with no diagnostic was the common, correct-looking mistake.
E_SRC='m=t;
f=main():i64{
  let x=str.concat("a";"b");
  <0
};'

emits   "E1 a real member of an unimported std.str is caught" E4033 3 - "${E_SRC}"
fixsays "E2 and the fix names the import" E4033 'add i=str:std.str; to imports' "${E_SRC}"

# E3 — i64/f64/bool are worse than str: no module of that name exists, so a
# fake member emitted an undefined C symbol NAMED AFTER IT at any arity, and a
# colliding runtime symbol would have linked and been called with the wrong
# signature.  No `fix`: there is nothing to import, so §3.1 says stay silent.
emits "E3 a member call on the i64 keyword is caught" E4033 3 - \
'm=t;
f=main():i64{
  let x=i64.frobnicate(1);
  <0
};'

# E4 — 136.63.  E4027's member check was disabled for any module exporting a
# type named like its own alias, which is the most natural alias a user would
# pick.  std.canvas exports a type `canvas`, and that was the entire reason.
emits "E4 alias colliding with an exported type is still checked" E4027 5 - \
'm=t;
i=canvas:std.canvas;
f=main():i64{
  let c=canvas.new("c";10;10);
  canvas.nosuchmemberatall(c);
  <0
};'

# ── F. Controls: the surfaces these changes could have broken ──────────────
echo ""
echo "F. controls — correct programs, and the diagnostics that already worked"

# F1 — a local binding legally shadows a primitive type keyword.  names.c
# seeds the primitives into module scope and scope_insert REPLACES a seed, so
# this is a legal program with a str receiver.  E1's check must not fire here.
accepts "F1 a local named 'str' is still a str receiver" \
'm=t;
f=main():i64{
  let str="hello";
  let n=str.len();
  <0
};'

runs "F1b and it computes the right length" '5' \
'm=t;
i=io:std.io;
f=main():i64{
  let str="hello";
  io.print("\(str.len())");
  <0
};'

# F2 — the sub-namespace prefixes.  `tpl.*` is not an import alias and looks
# exactly like a value receiver; it is recognised from the interface records.
accepts "F2 tpl.* sub-namespace under i=template" \
'm=t;
i=template:std.template;
f=main():i64{
  let t=tpl.parse("hi");
  <0
};'

# F3 — std.vec handle receivers.
accepts "F3 std.vec handle receiver" \
'm=t;
i=vec:std.vec;
f=main():i64{
  let v=vec.new();
  <0
};'

# F4 — a non-colliding alias for the same module E4 exercises.  This is C035's
# deliberate choice of `cv`, and it must keep working identically.
emits "F4 the cv alias still gets its E4027" E4027 5 - \
'm=t;
i=cv:std.canvas;
f=main():i64{
  let c=cv.new("c";10;10);
  cv.nosuchmemberatall(c);
  <0
};'

# F5 — E3011 for a genuinely undefined alias.  The 127.135 row claimed this
# was broken; it is not, and the correction inverted the row.  Pinned so the
# claim is not re-filed.
emits   "F5 an undefined alias is still E3011" E3011 3 - \
'm=t;
f=main():i64{
  let x=zzzqqq.concat("a";"b");
  <0
};'
# F6 — and E3011's edit-distance suggestion, the affordance 127.122 reuses for
# E4027. `zzzqqq` is deliberately NOT used here: it is beyond distance 3 of
# everything in scope, so no candidate is offered and none should be. A near
# miss of a real alias is what exercises the suggester.
fixsays "F6 and still suggests a near-miss alias" E3011 "did you mean 'io'?" \
'm=t;
i=io:std.io;
f=main():i64{
  ioo.print("x");
  <0
};'

# F7 — E4036's message, the one genuinely good diagnostic in this batch.
emits "F7 E4036 still explains the receiver-call mistake" E4036 - - \
'm=t;
f=pair(a:i64;b:str):i64{<a};
f=main():i64{
  let x=1;
  let y=x.pair("A");
  <0
};'

# ── G. 127.153 — E4036's fix, and the half of it that stays refused ────────
echo ""
echo "G. 127.153: E4036's fix is conditional, not unconditional"

# The row asked for this field to be filled with `pair(x;"A")` because "the
# compiler already located that function".  It knows such a function exists;
# it does not know it was MEANT — toke has no UFCS, so `x.pair(a)` and
# `pair(x;a)` share a name and nothing else, and 127.1 left the field empty
# for exactly that reason.
#
# G1 — where the module function's declared arity equals the receiver plus the
# supplied arguments, the rewrite is guaranteed well-formed and the receiver is
# a genuine candidate first argument.  The field is filled AND states the two
# numbers that justify it, so a consumer can check the reasoning.
fixsays "G1 arity matches: the fix names the direct call" E4036 \
    'it declares 2 parameters and this call supplies 2' \
'm=t;
f=pair(a:i64;b:str):i64{<a};
f=main():i64{
  let x=1;
  let y=x.pair("A");
  <0
};'

# G2 — where the arities DISAGREE the rewrite would not even compile, so the
# field stays absent.  This is the §3.1 guard and it is the case that would
# have been broken by doing what the row literally asked.
fixlacks "G2 arity disagrees: no fix is offered" E4036 'call it directly' \
'm=t;
f=pair(a:i64;b:str;c:i64):i64{<a};
f=main():i64{
  let x=1;
  let y=x.pair("A");
  <0
};'

# G3 — a member that matches no module function at all keeps an absent fix:
# a did-you-mean for an arbitrary member name is the 127.106 / 131.78 lesson.
fixlacks "G3 no such module function: no fix is offered" E4036 'call it directly' \
'm=t;
f=main():i64{
  let x=1;
  let y=x.nosuchthingatall("A");
  <0
};'

# F8 — plain numeric + is untouched.
runs "F8 numeric + still works" '7' \
'm=t;
i=io:std.io;
f=main():i64{ io.print("\(3+4)"); <0 };'

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
