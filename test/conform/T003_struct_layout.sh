#!/usr/bin/env bash
# T003_struct_layout.sh — a field or member is resolved against a layout the
# compiler has ESTABLISHED, or it is a diagnostic; never a plausible value.
#
# Five symptoms, one cause.  `struct_field_index()` answered 0 for "field not
# found" — indistinguishable from field 0 — and, when the base's own type was
# unknown, lowering adopted the first registered struct that happened to
# declare a field of that name.  Both answers are guesses that look like data:
#
#   127.86  std.securemem's SecureBuf is an opaque C handle (`"fields": []`).
#           `buf.size` GEP'd slot 0 — the first eight bytes of a `char id[24]`
#           — so buffers of 128/256/512 bytes reported sizes of 49/50/51: the
#           ASCII codes of the allocation ids "1", "2" and "3".
#   127.89  a field that does not exist on a `.tki`-imported type returned the
#           struct's FIRST field (the surviving half of 127.66, which closed
#           the annotated and direct-call spellings but not the `mt` one).
#   127.90  `Point.x` — the TYPE, not an instance — type-checked and lowered
#           to a load off a null base.
#   136.28  a value whose type was not established took another struct's
#           layout, so a documented field read landed in the wrong slot.
#   136.29  a struct literal naming a field that does not exist STORED over
#           field 0 — the mirror of 127.89, so neither reading nor writing a
#           struct field was checked.
#   136.36  a field name on an element of a STDLIB-returned struct array
#           resolved against an unrelated record: `.mean` on a `groupstat`
#           took `statsrow`'s slot 2, which in a groupstat is the SUM.  Two
#           numbers, no diagnostic.
#
# And the reason a layout so often could not be established was 127.80's own
# defect once more: imported_func_ret() matched the .tki's EXPORT spelling
# ("time.toparts") while every caller passes the CALL spelling ("toparts"), so
# the declared record return of the entire handwritten stdlib was dropped.
#
# Every negative case below is paired with a control that must keep compiling,
# because a false diagnostic on working code is worse than the missing check.
#
# Stories: 127.86, 127.89, 127.90, 136.28, 136.29, 136.36

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_structlayout_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "T003: a field resolves against an established layout, or it is loud"
echo "--------------------------------------"

# check_case NAME EXPECTED_RC EXPECTED_GREP SOURCE
#   Runs the full front end THROUGH LOWERING (not merely --check: the codegen
#   backstop speaks there), and asserts the exit code and, when EXPECTED_GREP
#   is non-empty, that the diagnostic stream matches it.  IR only, so a case
#   whose module has no implementation is not gated on the linker.
check_case() {
    local name="$1" exp_rc="$2" pattern="$3" src="$4"
    printf '%s\n' "${src}" > case.tk
    local out rc=0
    out="$("${TKC}" --diag-json -I . --emit-llvm -o case.ll case.tk 2>&1)" || rc=$?
    rm -f case.ll
    if [ "${exp_rc}" = "0" ] && [ "${rc}" != "0" ]; then
        echo "  FAIL: ${name}: expected a clean compile, got exit ${rc}"
        echo "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
        return
    fi
    if [ "${exp_rc}" != "0" ] && [ "${rc}" = "0" ]; then
        echo "  FAIL: ${name}: expected a diagnostic, compiled clean"
        FAIL=$((FAIL + 1))
        return
    fi
    if [ -n "${pattern}" ] && ! printf '%s' "${out}" | grep -q "${pattern}"; then
        echo "  FAIL: ${name}: diagnostics did not match [${pattern}]"
        echo "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# run_case NAME EXPECTED_STDOUT SOURCE
#   Compiles AND RUNS, asserting the program's output.  A value is the only
#   proof that a field read landed in the right slot: the wrong slot compiles
#   just as happily, which is the whole defect.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > run.tk
    local out rc=0
    out="$("${TKC}" --diag-json -I . -o run.bin run.tk 2>&1)" || rc=$?
    if [ "${rc}" != "0" ]; then
        echo "  FAIL: ${name}: did not compile (exit ${rc})"
        echo "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
        return
    fi
    local got
    got="$(./run.bin 2>&1)"
    rm -f run.bin
    if [ "${got}" != "${expected}" ]; then
        echo "  FAIL: ${name}: expected [${expected}], got [${got}]"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ir_case NAME EXPECTED_GREP SOURCE
#   Asserts on the EMITTED SLOT INDEX.  Some stdlib modules have no glue to
#   link against at all (136.32/136.34), so the IR is the only place their
#   field resolution can be pinned — and the slot is exactly what was wrong.
ir_case() {
    local name="$1" pattern="$2" src="$3"
    printf '%s\n' "${src}" > ir.tk
    local out rc=0
    out="$("${TKC}" --diag-json -I . --emit-llvm -o ir.ll ir.tk 2>&1)" || rc=$?
    if [ ! -f ir.ll ]; then
        echo "  FAIL: ${name}: no IR emitted (exit ${rc})"
        echo "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
        return
    fi
    if ! grep -q "${pattern}" ir.ll; then
        echo "  FAIL: ${name}: IR did not contain [${pattern}]"
        grep -oE 'i32 [0-9]+ ; \.[a-z]+' ir.ll | sed 's/^/      /' | head -5
        FAIL=$((FAIL + 1))
        rm -f ir.ll
        return
    fi
    rm -f ir.ll
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ── 127.86: a field the interface does not declare, on a type that came from
#    C.  SecureBuf's real layout is `char id[24]; int32 size; int64 expires`,
#    which is not the i64-slot layout toke indexes, so the interface declares
#    it opaque.  `b.size` therefore has no slot at all, and used to read the
#    id string's first eight bytes: 128 bytes of buffer reported as 49. ──
check_case "opaque C record: an undeclared field is E4025, not the id bytes" 1 \
    "struct 'SecureBuf' has no field 'size'" '
m=t;
i=io:std.io;
i=s:std.str;
i=sm:std.securemem;
f=main():i64{
  let b=sm.alloc(128;300);
  io.println(s.fromint(b.size));
  <0
};'

# Control: the same module used as documented still compiles and runs.  The
# withdrawal of the fields is the stopgap; the capability is not withdrawn.
check_case "opaque C record control: the declared surface still compiles" 0 "" '
m=t;
i=io:std.io;
i=sm:std.securemem;
f=main():i64{
  let b=sm.alloc(64;300);
  sm.write(b;"secret");
  io.println(sm.read(b));
  sm.wipe(b);
  <0
};'

# ── 127.90: a member of the TYPE, not of a value. ──
check_case "member access on a type name is E4033" 1 '"error_code":"E4033"' '
m=t;
i=io:std.io;
i=s:std.str;
t=Point{x:i64;y:i64};
f=main():i64{
  io.println(s.fromint(Point.x));
  <0
};'

check_case "E4033 names the type and the member" 1 \
    "'Point' is a type name, not a value: 'Point.x' has no meaning" '
m=t;
t=Point{x:i64;y:i64};
f=main():i64{
  let v=Point.x;
  <v
};'

# Control: the same field on an INSTANCE is a field access and must read the
# declared slot.  y is slot 1 — the value proves it is not slot 0.
run_case "type-name control: the field on an instance reads its own slot" \
    "$(printf '7\n9')" '
m=t;
i=io:std.io;
i=s:std.str;
t=Point{x:i64;y:i64};
f=main():i64{
  let p=Point{x:7;y:9};
  io.println(s.fromint(p.x));
  io.println(s.fromint(p.y));
  <0
};'

# ── 136.29: a struct literal naming a field that does not exist.  The old
#    compiler accepted `Point{x:1;y:2;z:3}` and stored 3 into slot 0, so
#    `p.x` read back 3.  The literal is the write side of 127.89. ──
check_case "struct literal with an unknown field is E4025" 1 \
    "struct 'Point' has no field 'z'" '
m=t;
i=io:std.io;
i=s:std.str;
t=Point{x:i64;y:i64};
f=main():i64{
  let p=Point{x:1;y:2;z:3};
  io.println(s.fromint(p.x));
  <0
};'

# Control: the fields the struct DOES declare, written out of declaration
# order, must still land in their own slots.
run_case "struct literal control: declared fields, written out of order" \
    "$(printf '1\n2\n3')" '
m=t;
i=io:std.io;
i=s:std.str;
t=Trip{a:i64;b:i64;c:i64};
f=main():i64{
  let v=Trip{c:3;a:1;b:2};
  io.println(s.fromint(v.a));
  io.println(s.fromint(v.b));
  io.println(s.fromint(v.c));
  <0
};'

# ── 127.89: the surviving half of 127.66.  A `.tki` export whose return is an
#    ERROR UNION was deliberately left unadopted by that story, so the value a
#    `mt` binds carries no layout — and a field that does not exist on it used
#    to compile and return the struct's first field. ──
cat > eumod.tk <<'EOF'
m=eumod;
t=Alpha{aid:str;score:i64};
t=Oops{msg:str};
f=mkalpha():Alpha!Oops{
  <Alpha{aid:"hello";score:77}
};
EOF
"${TKC}" --emit-interface --check eumod.tk >/dev/null 2>&1
if [ ! -f eumod.tki ]; then
    echo "  FAIL: setup: --emit-interface produced no eumod.tki"
    exit 1
fi

check_case "imported type through mt: a nonexistent field is not field 0" 1 \
    "struct 'Alpha' has no field 'nosuchfield'" '
m=t;
i=io:std.io;
i=s:std.str;
i=e:eumod;
f=main():i64{
  let r=e.mkalpha();
  let v=mt r {$ok:a a;$err:x Alpha{aid:"z";score:0}};
  io.println(s.fromint(v.nosuchfield));
  <0
};'

# ── 136.28: a value whose layout was never established, read as a field.
#    Two structs declare `shared`/`score` at DIFFERENT offsets.  Where the
#    layout IS on the page, it must now be established and the read must land
#    in the right slot; where it genuinely is not, it must be loud.

# `mk()` states `@(Aye)`, so the element is an Aye and `shared` is slot 1.
# The old compiler took the first struct declaring the name — `Bee`, slot 0 —
# and printed 11, the value of `alpha`.
run_case "an array-of-structs element resolves against its own layout" \
    "22" '
m=t;
i=io:std.io;
i=s:std.str;
t=Bee{shared:i64};
t=Aye{alpha:i64;shared:i64};
f=mk():@(Aye){
  <@(Aye{alpha:11;shared:22})
};
f=main():i64{
  io.println(s.fromint(mk().get(0).shared));
  <0
};'

# The same through a struct field declared as an array of structs — ooke reads
# exactly this shape, and got the right slot only because `$valerror` happens
# to be declared before `$validateerr`.
run_case "an array-of-structs FIELD element resolves against its own layout" \
    "$(printf 'f / M')" '
m=t;
i=io:std.io;
i=s:std.str;
t=Valerror{filepath:str;field:str;msg:str};
t=Validateerr{msg:str};
t=Valresult{errors:@Valerror};
f=mk():Valresult{
  <Valresult{errors:@(Valerror{filepath:"f";field:"g";msg:"M"})}
};
f=show(r:Valresult):i64{
  lp(let i=0;i<r.errors.len;i=i+1){
    let e=r.errors.get(i);
    io.println(s.concat(e.filepath;s.concat(" / ";e.msg)))
  };
  <0
};
f=main():i64{
  <show(mk())
};'

# Where the layout genuinely is NOT establishable — here the value arrives as
# a bare `i64` handle — the two candidate offsets disagree, and the answer is
# a diagnostic rather than whichever struct happened to be declared first.
# The old compiler emitted slot 0 (Bee), reading `alpha` where the source
# says `shared`.
check_case "unestablished layout with disagreeing offsets is E4034" 1 \
    '"error_code":"E4034"' '
m=t;
i=io:std.io;
i=s:std.str;
t=Bee{shared:i64};
t=Aye{alpha:i64;shared:i64};
f=read(h:i64):i64{
  <h.shared
};
f=main():i64{
  io.println(s.fromint(read(0)));
  <0
};'

check_case "E4034 says the offsets disagree" 1 \
    "declared at different offsets by more than one struct" '
m=t;
t=Bee{shared:i64};
t=Aye{alpha:i64;shared:i64};
f=read(h:i64):i64{
  <h.shared
};
f=main():i64{
  <read(0)
};'

# A field name NO struct declares, on a value with no established type, is the
# other half of the same residue: there is nothing to resolve it against, and
# slot 0 was simply invented.
check_case "a field no struct declares is E4034, not slot 0" 1 \
    "no struct in scope declares a field" '
m=t;
i=io:std.io;
i=s:std.str;
t=Point{x:i64;y:i64};
f=main():i64{
  let m=@("a":1);
  let v=m.get("a");
  io.println(s.fromint(v.nosuchfieldanywhere));
  <0
};'

# Control: when exactly one struct in scope declares the name, the fallback
# cannot change the answer, so it stays — and it must still read slot 1.
run_case "unambiguous field owner still resolves through an untracked base" \
    "22" '
m=t;
i=io:std.io;
i=s:std.str;
t=Aye{alpha:i64;shared:i64};
f=mk():@(Aye){
  <@(Aye{alpha:11;shared:22})
};
f=main():i64{
  io.println(s.fromint(mk().get(0).shared));
  <0
};'

# ── 136.36: an element of a STDLIB-returned struct array, with the field name
#    also declared by an unrelated type at another offset.  `SearchResult` is
#    `{id, score, payload}`; the decoy below declares `score` at index 2, and
#    the old compiler resolved the name against it — so `r.score` loaded the
#    payload POINTER and printed it as a double.  Both are numbers; nothing
#    looked wrong.
run_case "a stdlib struct-array element resolves against its own record" \
    "$(printf 'id=a\nscore=1')" '
m=t;
i=io:std.io;
i=s:std.str;
i=vs:std.vecstore;
t=Decoy{id:str;payload:str;score:f64};
f=main():i64{
  let st=mt vs.open("./t003vs.db"){$ok:v v;$err:e 0};
  let col=mt vs.collection(st;"c"){$ok:v v;$err:e 0};
  vs.upsert(col;"a";@(1.0;0.0);"payA");
  let res=vs.search(col;@(1.0;0.0);2;0.0);
  lp(let i=0;i<res.len;i=i+1){
    let r=res.get(i);
    io.println(s.concat("id=";r.id));
    io.println(s.concat("score=";s.fromfloat(r.score)))
  };
  vs.close(st);
  <0
};'

# The reported case: `analytics.groupstats` declares `[groupstat]!dferr`, and
# `groupstat` is `{group, count, sum, mean}` — `mean` is slot 3.  `statsrow`,
# declared first in the same interface, puts `mean` at slot 2, which in a
# groupstat is the SUM.  The old compiler emitted slot 2.  Asserted on the
# IR because this module has no glue to link (136.34).
ir_case "the reported statistics case emits the declared slot, not slot 2" \
    "i32 3 ; .mean" '
m=t;
i=io:std.io;
i=s:std.str;
i=analytics:std.analytics;
i=df:std.dataframe;
i=file:std.file;
f=main():i64{
  let raw=mt file.read("data.csv") {$ok:d d;$err:e ""};
  let data=mt df.fromcsv(raw) {$ok:d d;$err:e df.fromrows(@();@())};
  let gs=mt analytics.groupstats(data;"r";"v") {$ok:g g;$err:e @()};
  lp(let i=0;i<gs.len;i=i+1){
    let g=gs.get(i);
    io.println(s.concat(g.group;s.concat(" ";s.concat(s.fromfloat(g.sum);s.concat(" ";s.fromfloat(g.mean))))))
  };
  <0
};'

# ── The lookup key that made all of this reachable.  A handwritten stdlib
#    .tki namespaces its exports ("time.toparts"); the call writes
#    "tm.toparts(0)".  imported_func_ret() matched the export spelling, so
#    every such record return was untyped and E4025 never ran on it. ──
check_case "a std record return is typed, so a bogus field on it is E4025" 1 \
    "struct 'TimeParts' has no field 'yearr'" '
m=t;
i=tm:std.time;
f=main():i64{
  let p=tm.toparts(0);
  <p.yearr
};'

# Control: the declared fields of that same return must keep compiling, and
# keep reading their own slots — this is the false-positive guard for the
# lookup-key change.
run_case "std record return control: declared fields read their own slots" \
    "$(printf '1970\n1\n1')" '
m=t;
i=io:std.io;
i=s:std.str;
i=tm:std.time;
f=main():i64{
  let p=tm.toparts(0);
  io.println(s.fromint(p.year));
  io.println(s.fromint(p.month));
  io.println(s.fromint(p.day));
  <0
};'

# Control: `v.get(i)` on an imported record type is a declared METHOD spelled
# as a subscript, not an index.  Typing those returns for the first time made
# this reach the "cannot index into" check and reject working code; the
# narrowing that removed that false positive is pinned here.
check_case "subscript-spelled method on an imported record still compiles" 0 "" '
m=t;
i=io:std.io;
i=s:std.str;
i=v:std.vec;
f=main():i64{
  let bits=v.new();
  bits.push(7);
  io.println(s.fromint(bits.get(0)));
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
