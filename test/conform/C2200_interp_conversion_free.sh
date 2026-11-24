#!/usr/bin/env bash
# C2200_interp_conversion_free.sh — a string interpolation frees the number
# conversions it made once tk_str_join_n has copied them (story 127.299).
#
# THE DEFECT.  Generated code frees nothing (127.299; memory-model.md §1.1
#   promises per-function arenas that were never built).  The cheapest class
#   of leak is one no toke value ever sees: in `"k\(i)=\(x)"` each number is
#   converted by tk_str_fromi64_w / tk_str_fromfloat_w (str_from_int /
#   str_from_float: a fresh malloc(24) / malloc(64)), the conversions are
#   passed to ONE tk_str_join_n, which copies them into a new string — and the
#   conversions stayed allocated forever.  A loop building 1M such strings
#   held 112 MB of dead conversions (162 MB peak RSS; 3,000,198 mallocs, 14
#   frees).
#
# THE FIX (the one slice docs/decisions/127.299-function-arena.md finds
#   provably safe).  NODE_STR_LIT lowering records, per segment, the SSA temp
#   of a conversion it emitted itself, and after the literal's tk_str_join_n
#   emits `call void @free` for exactly those temps.  The temp is unnamed (no
#   local, store, argument or return can hold it), its producer always
#   returns fresh malloc'd memory (never a literal, a sealed string or one of
#   its arguments), and join_n copies every part and returns a new sealed
#   block, keeping no pointer to any part.  Not freed: a one-segment literal
#   (`"\(n)"` — the conversion IS the result), a u64 / u8 / u16 / u32 operand
#   rendered by the >= 2^63 select (the value is a select, not the temp), a
#   bool (a literal), a string operand (the caller's value).
#
# WHAT IS ASSERTED.
#   Part 1 (--emit-llvm): which conversions are freed, and that every free
#     follows the literal's join_n (one free per int/float segment of a
#     >= 2-segment literal; none for the excluded shapes above).
#   Part 2 (end to end, program and stdlib built through an ASan TKC_CC
#     wrapper, under TK_STR_VERIFY=1): exact output for every shape — signed,
#     negative, float, i32, u8, u64 max, bool, str, a literal whose segment
#     sits behind an overflow-check branch, interpolated results kept in an
#     array and read after the loop, a one-segment literal — and no ASan
#     report.  A wrong free (the join result, a literal, a sealed string, a
#     u64 select, a conversion still needed) is an ASan "attempting free on
#     address which was not malloc()-ed", "double-free" or
#     "heap-use-after-free" here.
#
# Negative control (measured 2026-11-23 on feature/127.299-fn-arena): a
#   scratch tkc that also frees the one-segment conversion (the `nvals == 1`
#   case) fails Part 2 with "heap-use-after-free" (READ in puts, from
#   tk_io_println_w), and Part 1 (7 frees, one not after a join_n); against
#   the base a4636b56 Part 1 fails (no free emitted) and Part 2 passes.
#
# Story: 127.299
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C2200_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2200_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C2200: interpolation frees its number conversions after join_n (127.299)"
echo "---------------------------------------------------------------------------"
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
f=main():i64{
  let n=-42;
  let x=2.5;
  let k=7 as i32;
  let b=200 as u8;
  let u=(0-1) as u64;
  let s="str";
  let t=true;
  let keep=mut.@();
  lp(let i=0;i<2000;i=i+1){
    keep=keep.append("i=\(i);n=\(n*i)")
  };
  (io.println("a\(n)b\(x)c\(k)d\(b)"));
  let one="\(n)";
  (io.println(one));
  (io.println("u=\(u)"));
  (io.println("s=\(s) t=\(t)"));
  let inner="in\(n)";
  (io.println("o\(inner)o"));
  (io.println("\(keep.get(0))|\(keep.get(1))|\(keep.get(1999))"));
  <0
};
TOKE

# ── Part 1: IR shape ─────────────────────────────────────────────────────
if ! "${TKC}" --emit-llvm p.tk --out p.ll > ir.log 2>&1; then
    bad "p.tk did not lower"; head -4 ir.log | sed 's/^/        /'
else
    nfree=$(grep -c 'call void @free(i8\* %t[0-9]*) ; 127.299' p.ll)
    # loop literal: i, n*i (2); line 1: n, x, k (3; u8 is the select path);
    # inner: n (1); the keep line's parts are strings (0); "\(n)", u=, s=/t= (0).
    if [ "${nfree}" -eq 6 ]; then ok "6 conversion frees emitted (got ${nfree})"
    else bad "expected 6 conversion frees, got ${nfree}"; fi
    # Every freed temp is a conversion result and an argument of the join_n
    # immediately above it.
    if awk '
        /= inttoptr i64 %t[0-9]+ to i8\*/ { conv[$1] = 1 }
        /@tk_str_join_n\(/ { args = $0; next_ok = 1; next }
        /call void @free\(i8\* %t[0-9]+\) ; 127.299/ {
            match($0, /%t[0-9]+/); v = substr($0, RSTART, RLENGTH)
            if (!next_ok || !(v in conv) || (index(args, "i8* " v ",") == 0 && index(args, "i8* " v ")") == 0)) { bad = 1 }
            next
        }
        { next_ok = 0 }
        END { exit bad }' p.ll; then
        ok "each free names a conversion passed to the join_n just above it"
    else
        bad "a free does not directly follow a join_n that took that conversion"
    fi
    # The one-segment literal and the u64 select keep their value.
    if grep -A1 '@tk_str_join_n(i64 2, i8\* %t[0-9]*, i8\* %t[0-9]*) ; u64' p.ll | grep -q '127.299'; then
        bad "the u64 >= 2^63 helper join frees a part"
    else
        ok "the u64 >= 2^63 select path frees nothing"
    fi
fi

# ── Part 2: end to end under ASan ────────────────────────────────────────
CLANG="$(command -v clang || true)"
if [ -z "${CLANG}" ]; then
    bad "clang not on PATH for the ASan TKC_CC wrapper"
else
    SANF="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
    printf '#!/bin/sh\nexec "%s" "$@" %s -g\n' "${CLANG}" "${SANF}" > asancc
    chmod +x asancc
    want='a-42b2.5c7d200
-42
u=18446744073709551615
s=str t=true
oin-42o
i=0;n=0|i=1;n=-42|i=1999;n=-83958'
    if ! TKC_CC="${WORK}/asancc" TKC_STDLIB_DIR="${S}" "${TKC}" --out p p.tk > p.log 2>&1; then
        bad "p.tk did not compile under ASan"; grep -v '"severity":"warning"' p.log | head -4 | sed 's/^/        /'
    else
        got="$(TK_STR_VERIFY=1 ./p 2>&1)"; rc=$?
        if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
            ok "exact output, no ASan report (TK_STR_VERIFY=1)"
        else
            bad "rc=${rc}; expected the six lines, got:"
            printf '%s\n' "${got}" | grep -a -m4 'ERROR\|runtime error\|=\|^-\|^o' | sed 's/^/        /'
        fi
    fi
fi

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
