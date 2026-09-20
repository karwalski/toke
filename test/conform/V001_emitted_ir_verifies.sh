#!/usr/bin/env bash
# V001_emitted_ir_verifies.sh — the LLVM IR the compiler emits must VERIFY
# (story 127.93).
#
# Nothing in the toolchain checked this.  `--check` stops at the front end and
# `--emit-llvm` writes whatever lowering produced, so a program could pass both
# and still be IR that no LLVM will accept — the failure surfaced only when
# clang ran the verifier, one stage past everything we gate on.  Four
# documented examples had been shipping in that state (found by 136.26 once the
# documentation gate began LINKING rather than type-checking):
#
#   reference/type-system.md  [block 0]   '%t6'  i64 but expected 'i8'
#   reference/type-system.md  [block 22]  '%t3'  i64 but expected 'i8'
#   reference/expressions.md  [block 21]  '%t7'  i64 but expected 'i32'
#   stdlib/db.md              [block 6]   '%t36' double but expected 'i64'
#
# Two mechanisms, both the same shape as 127.80 and 127.86 — a type the
# compiler STATED standing in for one it never established:
#
#   1. NARROWING STORE.  `let b:u8 = big as u8;` — the bind lowering passed
#      `has_ann ? vty : expr_llvm_type(init)` as the SOURCE type of the
#      coercion, i.e. it assumed the annotation described the initialiser.  It
#      does not: a sub-64-bit cast truncs and re-extends to i64 for storage
#      (80.2.1).  So the coercion became a no-op and an i64 value was stored
#      into an i8 slot.  The initialiser's real type is now asked for.
#
#   2. INTEGER COMPARE ON A DOUBLE.  `mt row.f64(r;"price")` — the list of
#      stdlib SUB-namespaces ("row", from std.db) sat inline in emit_expr and
#      nowhere else, so expr_llvm_type could not resolve `row.f64` at all and
#      answered i64 for a wrapper emit_expr bitcasts to double.  The match then
#      emitted `icmp ne i64 <a double>`.  One resolver, asked by both.
#
# Every case below asserts that the emitted IR passes an LLVM VERIFIER, which
# is the thing that was never run.  Several also assert the RESULT, because
# IR that merely verifies can still be wrong: case 2 additionally read the f64
# bit-pattern back through `sitofp`, so the number was garbage either way.
#
# Story: 127.93

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

# ── Locate a verifier ────────────────────────────────────────────────────────
# llvm-as + `opt -passes=verify` is the direct spelling (test/verify_ir.sh uses
# it), but neither ships with Apple's command-line tools.  `clang -x ir -c`
# runs the same verifier on the same module and is present wherever tkc can
# build at all, so it is the fallback rather than a reason to skip.
VERIFY_KIND=""
LLVM_AS="" OPT=""
for sfx in "" "-21" "-20" "-19" "-18" "-17" "-16" "-15" "-14"; do
    if [ -z "${LLVM_AS}" ] && command -v "llvm-as${sfx}" >/dev/null 2>&1; then LLVM_AS="llvm-as${sfx}"; fi
    if [ -z "${OPT}" ]    && command -v "opt${sfx}"    >/dev/null 2>&1; then OPT="opt${sfx}"; fi
done
if [ -n "${LLVM_AS}" ] && [ -n "${OPT}" ]; then
    VERIFY_KIND="opt"
elif command -v clang >/dev/null 2>&1; then
    VERIFY_KIND="clang"
else
    echo "SKIP: no LLVM verifier found (need llvm-as+opt, or clang)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_irverify_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "V001: the emitted LLVM IR verifies (verifier: ${VERIFY_KIND})"
echo "--------------------------------------"

# verify_ll FILE — 0 if the module passes the LLVM verifier, else 1; the
# verifier's own complaint is left in verify.log.
verify_ll() {
    local ll="$1"
    if [ "${VERIFY_KIND}" = "opt" ]; then
        "${LLVM_AS}" "${ll}" -o mod.bc >verify.log 2>&1 || return 1
        "${OPT}" -passes=verify mod.bc -o /dev/null >verify.log 2>&1 || return 1
    else
        # -Wno-override-module: the .ll carries its own triple, which clang
        # warns about and which is not what is under test here.
        clang -Wno-override-module -x ir -c "${ll}" -o mod.o >verify.log 2>&1 || return 1
    fi
    return 0
}

# ir_verifies NAME SOURCE — emit IR and require that it verifies.
ir_verifies() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > v.tk
    rm -f v.ll mod.o mod.bc
    local rc=0
    "${TKC}" --allow-all --diag-json --emit-llvm -o v.ll v.tk >emit.log 2>&1 || rc=$?
    if [ ! -f v.ll ]; then
        echo "  FAIL: ${name}: no IR emitted (exit ${rc})"
        sed 's/^/      /' emit.log | head -3
        FAIL=$((FAIL + 1))
        return
    fi
    if ! verify_ll v.ll; then
        echo "  FAIL: ${name}: emitted IR does not verify"
        sed 's/^/      /' verify.log | head -4
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# run_case NAME EXPECTED_STDOUT SOURCE — IR that verifies can still compute the
# wrong number; these pin the value as well.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > r.tk
    rm -f r.bin
    local out rc=0
    out="$("${TKC}" --allow-all --diag-json -o r.bin r.tk 2>&1)" || rc=$?
    if [ ! -x r.bin ]; then
        echo "  FAIL: ${name}: did not compile (exit ${rc})"
        echo "${out}" | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
        return
    fi
    local got
    got="$(./r.bin 2>&1)"
    rm -f r.bin
    if [ "${got}" != "${expected}" ]; then
        echo "  FAIL: ${name}: expected [${expected}], got [${got}]"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ── The four reproducers, verbatim from the pages that shipped them.  Each is
#    given an entry point so it is a complete program; nothing else is changed.

# docs/reference/type-system.md, block 22.
ir_verifies "narrowing store: let byte:u8 = big as u8 (type-system.md 22)" '
m=test;
f=example():void{
  let big:i64 = 300;
  let byte:u8 = big as u8;
};
f=main():i64{ <0 };'

# docs/reference/type-system.md, block 0 — the same store inside the primitive
# tour, so the very first example on the type-system page was unbuildable.
ir_verifies "narrowing store: the primitives tour (type-system.md 0)" '
m=primitives;
f=demo():void{
  let age:i64 = 30;
  let ratio:f64 = 1.618;
  let flag:bool = true;
  let name:$str = "toke";
  let byte:u8 = 255 as u8;
};
f=main():i64{ <0 };'

# docs/reference/expressions.md, block 21 — the 32-bit slot, same mechanism.
ir_verifies "narrowing store: the cast table (expressions.md 21)" '
m=exprs;
f=demo():i64{
  let x:i64=42;
  let y:f64=x as f64;
  let z:u64=x as u64;
  let w:i32=x as i32;
  < x
};
f=main():i64{ <0 };'

# docs/stdlib/db.md, block 6 — `icmp` applied to a double.
ir_verifies "integer compare on a double: mt row.f64 (db.md 6)" '
m=app;
i=db:std.db;
f=getprice():f64{
  let r=db.one("SELECT price FROM t WHERE id=?";@("1"));
  let row=mt r {$ok:v v;$err:e $row{cols:@()}};
  let price=mt row.f64(row;"price") {$ok:f f;$err:e 0.0};
  < price
};
f=main():i64{ <0 };'

# ── The narrowing store, widened over every slot width and both signednesses,
#    since the defect was in the shared bind path and not in any one width. ──
for W in i8 u8 i16 u16 i32 u32; do
    ir_verifies "narrowing store verifies for :${W}" "
m=w;
f=demo():i64{
  let big:i64 = 300;
  let n:${W} = big as ${W};
  < 0
};
f=main():i64{ <0 };"
done

# ── The VALUE, not only the verifier.  300 as u8 is 44; the old lowering
#    stored the untruncated i64 into the i8 slot, so had the IR been accepted
#    the byte would have been whatever the low bits happened to be. ──
run_case "narrowing store keeps the narrowed value" \
    "$(printf '44\n300')" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let big:i64 = 300;
  let byte:u8 = big as u8;
  io.println(s.fromint(byte as i64));
  io.println(s.fromint(big));
  <0
};'

run_case "narrowing store to i32 keeps the narrowed value" \
    "$(printf '42\n-1')" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let x:i64=42;
  let w:i32=x as i32;
  let neg:i32=(0-1) as i32;
  io.println(s.fromint(w as i64));
  io.println(s.fromint(neg as i64));
  <0
};'

# ── Controls: the annotated bind is the language's ordinary spelling, and the
#    coercion change must not disturb the types that already matched. ──
run_case "control: annotated i64/f64/\$str/bool binds are unchanged" \
    "$(printf '30\n1.618\ntoke\nflag')" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let age:i64 = 30;
  let ratio:f64 = 1.618;
  let name:$str = "toke";
  let flag:bool = true;
  io.println(s.fromint(age));
  io.println(s.fromfloat(ratio));
  io.println(name);
  if(flag){
    io.println("flag")
  };
  <0
};'

ir_verifies "control: an annotated struct bind still verifies" '
m=t;
i=io:std.io;
i=s:std.str;
t=Point{x:i64;y:i64};
f=mk():Point{ <Point{x:7;y:9} };
f=main():i64{
  let p:Point = mk();
  io.println(s.fromint(p.y));
  <0
};'

# Control for mechanism 2: the OTHER std.db row accessors keep their own ABI.
# row.i64 is an i64 wrapper and must not acquire a float arm test.
ir_verifies "control: the sibling row.i64 accessor still verifies" '
m=app;
i=db:std.db;
f=getid():i64{
  let r=db.one("SELECT id FROM t";@());
  let row=mt r {$ok:v v;$err:e $row{cols:@()}};
  let id=mt row.i64(row;"id") {$ok:n n;$err:e 0-1};
  < id
};
f=main():i64{ <0 };'

ir_verifies "control: row.str keeps its string ABI" '
m=app;
i=db:std.db;
i=io:std.io;
f=getname():i64{
  let r=db.one("SELECT name FROM t";@());
  let row=mt r {$ok:v v;$err:e $row{cols:@()}};
  let nm=mt row.str(row;"name") {$ok:v v;$err:e "?"};
  io.println(nm);
  < 0
};
f=main():i64{ <0 };'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
