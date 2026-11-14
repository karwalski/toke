#!/usr/bin/env bash
# C1780_ir_pie_module_flags.sh — every module tkc emits says it is PIE code
# (story 127.439).
#
# THE DEFECT.  tkc's IR carried no "PIC Level" module flag, so LLVM compiled
# it as non-PIC code.  Every function has the sspstrong attribute (124.2e), and
# on aarch64 glibc the canary is read from the global __stack_chk_guard.  As
# non-PIC code that read was a direct page-relative reference
# (R_AARCH64_ADR_PREL_PG_HI21).  Ubuntu's clang links a PIE by default, so
# every toke program with a stack buffer failed to link on linux-arm64 with
# "relocation ... can not be used when making a shared object; recompile
# with -fPIC".  Passing -fPIE / -fPIC to the driver does not change an IR
# input; only the module flag does.
#
#   1. --emit-llvm output carries !"PIC Level" 2 and !"PIE Level" 2
#   2. so does -g output, beside the two DWARF flags, in one
#      !llvm.module.flags list
#   3. the -g module still compiles (the ids do not collide)
#   4. compiled for aarch64-linux, the canary is reached through the GOT
#      (R_AARCH64_ADR_GOT_PAGE), never R_AARCH64_ADR_PREL_PG_HI21.  This
#      case needs an objdump that names aarch64 relocation types (llvm-objdump
#      anywhere; GNU objdump only on an aarch64 host); the others always run.
#
# Negative control: on the parent commit cases 1, 2 and 4 fail (no flag;
# the canary relocation is ADR_PREL_PG_HI21).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1780_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
echo "C1780: emitted IR carries PIC/PIE Level module flags (127.439)"
echo "------------------------------------------------------------------------"
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

cat > p.tk <<'EOF'
m=p;
i=io:std.io;
f=main():i64{
  let a=@(1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16);
  (io.println("ok"));
  <a.len-16
};
EOF

if "${TKC}" p.tk --emit-llvm --out p.ll >c1.log 2>&1 \
   && grep -q '!"PIC Level", i32 2' p.ll && grep -q '!"PIE Level", i32 2' p.ll \
   && grep -q '^!llvm.module.flags' p.ll; then
    ok "1 --emit-llvm: PIC Level 2 and PIE Level 2"
else
    bad "1 --emit-llvm lacks the PIC/PIE Level flags"; sed 's/^/      | /' c1.log | head -5
fi

if "${TKC}" p.tk -g --emit-llvm --out pg.ll >c2.log 2>&1 \
   && [ "$(grep -c '^!llvm.module.flags' pg.ll)" = 1 ] \
   && grep -q '!"PIE Level", i32 2' pg.ll && grep -q '!"Dwarf Version"' pg.ll; then
    ok "2 -g: one module.flags list with PIC/PIE Level and the DWARF flags"
else
    bad "2 -g module flags wrong"; grep -n 'module.flags\|Level\|Version' pg.ll 2>/dev/null | head -6 | sed 's/^/      | /'
fi

if "${TKC}" p.tk -g --out pg >c3.log 2>&1 && ./pg | grep -qx ok; then
    ok "3 the -g program builds and runs"
else
    bad "3 the -g program does not build"; sed 's/^/      | /' c3.log | head -5
fi

# 127.439: an objdump counts only if it can NAME aarch64 relocations.  GNU
# objdump on an x86-64 Linux host reads the aarch64 ELF but prints every
# relocation type as UNKNOWN, so case 4 failed there although the canary
# relocation was right.  Candidates: llvm-objdump on PATH, the one beside the
# clang that compiles the probe (Ubuntu keeps it in /usr/lib/llvm-N/bin), then
# objdump; the first that names a type in the probe's relocations is used.
OD_CANDIDATES=()
for c in llvm-objdump "$(clang -print-prog-name=llvm-objdump 2>/dev/null)" objdump; do
    case "$c" in
        /*) [ -x "$c" ] && OD_CANDIDATES+=("$c") ;;
        ?*) command -v "$c" >/dev/null 2>&1 && OD_CANDIDATES+=("$c") ;;
    esac
done
# tkc's own module (its flags, its #0 = sspstrong attribute group) plus one
# probe function that has a stack buffer, so a canary is certainly emitted.
OD=""
if [ "${#OD_CANDIDATES[@]}" -gt 0 ] \
   && "${TKC}" p.tk --emit-llvm --target aarch64-unknown-linux-gnu --out pa.ll >c4.log 2>&1 \
   && printf '%s\n' 'declare void @c1780_use(ptr)' \
        'define void @c1780_probe() #0 {' '  %b = alloca [64 x i8]' \
        '  call void @c1780_use(ptr %b)' '  ret void' '}' >> pa.ll \
   && clang -Wno-override-module -target aarch64-unknown-linux-gnu -O1 -c pa.ll -o pa.o >>c4.log 2>&1; then
    for c in "${OD_CANDIDATES[@]}"; do
        if "$c" -r pa.o >pa.rel 2>>c4.log && grep -q stack_chk_guard pa.rel \
           && grep stack_chk_guard pa.rel | grep -q R_AARCH64_; then OD="$c"; break; fi
    done
    [ -n "${OD}" ] || echo "  note: no objdump here names aarch64 relocations (tried: ${OD_CANDIDATES[*]})"
fi
if [ -n "${OD}" ]; then
    if grep stack_chk_guard pa.rel | grep -q ADR_PREL_PG_HI21; then
        bad "4 aarch64: the canary is addressed directly (ADR_PREL_PG_HI21)"
    elif grep stack_chk_guard pa.rel | grep -q GOT; then
        ok "4 aarch64: the canary is reached through the GOT"
    else
        bad "4 aarch64: unexpected canary relocation"; grep stack_chk_guard pa.rel | head -3 | sed 's/^/      | /'
    fi
else
    echo "  note: case 4 not run here (no objdump that names aarch64 relocations, or no aarch64 clang target)"
fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
