#!/usr/bin/env bash
# C019_error_box_not_leaked.sh — an error return does not allocate, and the
# error channel's memory is bounded (stories 127.109 and 127.49).
#
# Every error return, and every `!` propagation, used to call malloc() for a
# 16-byte box, and the emitted code contained NO free at all.  So a program
# leaked 16 bytes per error EVER raised.  Measured on patterns/err-default
# (`<mt chk(x){$ok:v v;$err:e -1}` where chk fails on every 7th input):
#
#     N            mallocs      bytes malloc'd   frees   peak RSS
#     1,000            175           24,466        14
#     7,000          1,032           38,180        14
#     64,000,000  9,142,890      146,307,916        14    148,389,888
#
# 9,142,890 - 32 baseline = 9,142,858 = exactly the number of multiples of 7
# below 64e6, i.e. exactly one box per error return.  Times 16 bytes =
# 146,285,728.  The free count is CONSTANT in N — it is the libc startup
# baseline — so this was a pure leak, not allocator churn.
#
# That also FALSIFIES 127.49's original premise, which said "the $ok path
# appears to heap-allocate per call": with the error condition made
# unreachable the same program allocates 32 (constant to N=1e6).  The $ok path
# allocates nothing.  It was never the success path and never allocator churn.
#
# THE FIX IS AN OWNERSHIP RULE, NOT A free().  Each thread owns ONE error-box
# buffer (tk_err_box, tk_runtime.c), grown on demand and reused by every raise
# on that thread.  A box is valid until the next raise on the same thread —
# which is exactly the window runtime-abi.md §7.5 restriction 1 ALREADY
# documents for the $err arm's binding.  So the rule adds no restriction: it
# makes the allocation's lifetime equal to the reference's already-documented
# lifetime.  There is then nothing to free per error at all.
#
# Stories: 127.109, 127.49

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

WORK="$(mktemp -d /tmp/tkc_c019_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C019: the error box is not allocated per error and not leaked"
echo "--------------------------------------"

# ── Part 1: IR shape.  Portable, exact, and the part that pins the mechanism. ──

# ir_has NAME PATTERN SOURCE / ir_hasnt NAME PATTERN SOURCE
_emit_ir() {
    printf '%s\n' "$1" > ir.tk
    rm -f ir.ll
    "${TKC}" --allow-all --diag-json --emit-llvm -o ir.ll ir.tk >/dev/null 2>&1
    [ -f ir.ll ]
}
ir_has() {
    local name="$1" pattern="$2" src="$3"
    if ! _emit_ir "${src}"; then
        echo "  FAIL: ${name}: no IR emitted"; FAIL=$((FAIL + 1)); return
    fi
    if grep -q -- "${pattern}" ir.ll; then
        echo "  PASS: ${name}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: IR did not contain [${pattern}]"; FAIL=$((FAIL + 1))
    fi
}
ir_hasnt() {
    local name="$1" pattern="$2" src="$3"
    if ! _emit_ir "${src}"; then
        echo "  FAIL: ${name}: no IR emitted"; FAIL=$((FAIL + 1)); return
    fi
    if grep -q -- "${pattern}" ir.ll; then
        echo "  FAIL: ${name}: IR still contains [${pattern}]"
        grep -n -- "${pattern}" ir.ll | sed 's/^/      /' | head -3
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: ${name}"; PASS=$((PASS + 1))
    fi
}

SUM_ERR='
m=t;
i=io:std.io;
t=$myerr{$bad:bool};
f=chk(x:i64):i64!$myerr{
  if(x==0){<$myerr{$bad:true}};
  <x*2
};
f=main():i64{
  let r=mt chk(0){$ok:v v;$err:e 0-1};
  io.println("done");
  <0
};'

REC_ERR='
m=t;
i=io:std.io;
t=$myerr{code:i64;msg:$str};
f=chk(x:i64):i64!$myerr{
  if(x==0){<$myerr{code:1;msg:"zero"}};
  <x*2
};
f=main():i64{
  let r=mt chk(0){$ok:v v;$err:e 0-1};
  io.println("done");
  <0
};'

ir_has "a sum-typed error box comes from the per-thread error buffer" \
    "@tk_err_box(i64 16) ; sum_lit myerr" "${SUM_ERR}"
ir_hasnt "a sum-typed error box does not call malloc" \
    "@malloc(i64 16) ; sum_lit myerr" "${SUM_ERR}"
ir_has "a record-typed error box comes from the per-thread error buffer" \
    "@tk_err_box(i64 16) ; struct_lit myerr" "${REC_ERR}"
ir_hasnt "a record-typed error box does not call malloc" \
    "@malloc(i64 16) ; struct_lit myerr" "${REC_ERR}"

# The `!` propagation path synthesised its own 16-byte box (127.56) and leaked
# it exactly like the returned one.
PROP='
m=t;
i=io:std.io;
i=file:std.file;
t=$myerr{$bad:bool};
f=rd(p:$str):$str!$myerr{
  let s=file.read(p)!$myerr;
  <s
};
f=main():i64{
  let r=mt rd("c019_no_such_file.txt"){$ok:v v;$err:e "ERR"};
  io.println(r);
  <0
};'
ir_has "the propagated box comes from the per-thread error buffer" \
    "@tk_err_box(i64 16) ; 127.56 propagated" "${PROP}"
ir_hasnt "the propagated box does not call malloc" \
    "@malloc(i64 16) ; 127.56 propagated" "${PROP}"

# ── Control: the change is scoped to ERROR boxes.  An ordinary struct literal
#    is a value with an ordinary lifetime and must keep its own malloc — giving
#    every struct in the language the error buffer would be a far worse defect
#    than the one fixed, because two live structs would alias. ──
PLAIN='
m=t;
i=io:std.io;
i=s:std.str;
t=$point{x:i64;y:i64};
f=main():i64{
  let a=$point{x:1;y:2};
  let b=$point{x:3;y:4};
  io.println(s.fromint(a.x+b.x));
  <0
};'
ir_has "control: an ordinary struct literal still uses malloc" \
    "@malloc(i64 16) ; struct_lit point" "${PLAIN}"
ir_hasnt "control: an ordinary struct literal does not use the error buffer" \
    "@tk_err_box" "${PLAIN}"

# Control: a NON-error struct literal returned from a FALLIBLE function is a
# success value, not the error box, so it keeps malloc.  This is the case the
# one-shot flag could have stolen.
OKSTRUCT='
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64};
t=$point{x:i64;y:i64};
f=mk(n:i64):$point!$myerr{
  if(n<0){<$myerr{code:1}};
  <$point{x:n;y:n}
};
f=main():i64{
  let p=mt mk(5){$ok:v v;$err:e $point{x:0;y:0}};
  io.println(s.fromint(p.x));
  <0
};'
ir_has "control: an ok-path struct literal in a fallible fn still uses malloc" \
    "@malloc(i64 16) ; struct_lit point" "${OKSTRUCT}"
ir_has "control: and that fn's error box still uses the error buffer" \
    "@tk_err_box(i64 8) ; struct_lit myerr" "${OKSTRUCT}"

# ── Part 2: the allocation count itself.  This is the story's measurement, so
#    it is made with a counter, not inferred.  Darwin-only: it needs dyld
#    interposition. ──

if [ "$(uname -s)" != "Darwin" ]; then
    echo "  SKIP: allocation count needs dyld interposition (Darwin only)"
else

cat > mc.c <<'MCEOF'
/* headless malloc/free counter via dyld interposition (no debugger, no UI) */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
static _Atomic uint64_t n_malloc = 0, n_free = 0;
static _Atomic uint64_t b_malloc = 0;
static void mc_report(void) {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "MC %llu %llu %llu\n",
        (unsigned long long)n_malloc, (unsigned long long)n_free,
        (unsigned long long)b_malloc);
    write(2, buf, (size_t)n);
}
__attribute__((constructor)) static void mc_init(void) { atexit(mc_report); }
void *mc_malloc(size_t s)           { n_malloc++; b_malloc += s; return malloc(s); }
void  mc_free(void *p)              { if (p) n_free++; free(p); }
__attribute__((used)) static struct { const void *repl; const void *orig; }
interposers[] __attribute__((section("__DATA,__interpose"))) = {
    { (const void*)mc_malloc, (const void*)malloc },
    { (const void*)mc_free,   (const void*)free   },
};
MCEOF

if ! cc -dynamiclib -O2 -o libmc.dylib mc.c 2>/dev/null; then
    echo "  SKIP: could not build the allocation counter"
else

# count BINARY N -> "mallocs frees bytes"
count() {
    PAT_N="$2" DYLD_INSERT_LIBRARIES=./libmc.dylib "$1" 2>&1 >/dev/null \
        | awk '/^MC /{print $2, $3, $4}'
}

# ── The counter is a gate, so prove it can SEE allocations before trusting a
#    zero from it.  A probe with a known, parameterised count: if the counter
#    is broken or interposition is not in effect, the deltas are 0 and this
#    fails rather than silently reporting "no allocations". ──
cat > probe.c <<'PBEOF'
#include <stdlib.h>
int main(void) {
    const char *e = getenv("PAT_N");
    long n = e ? atol(e) : 0;
    for (long i = 0; i < n; i++) { void *p = malloc(16); (void)p; }
    return 0;
}
PBEOF
if ! cc -O0 -o probe probe.c 2>/dev/null; then
    echo "  FAIL: could not build the counter's own probe"
    FAIL=$((FAIL + 1))
else
    p0="$(count ./probe 0)";    m0="$(echo "${p0}" | cut -d' ' -f1)"
    p1="$(count ./probe 1000)"; m1="$(echo "${p1}" | cut -d' ' -f1)"
    if [ -z "${m0}" ] || [ -z "${m1}" ]; then
        echo "  FAIL: gate check: the counter produced no reading (interposition not in effect?)"
        FAIL=$((FAIL + 1))
    elif [ "$((m1 - m0))" -ne 1000 ]; then
        echo "  FAIL: gate check: counter is not exact — 1000 known mallocs read as $((m1 - m0))"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: gate check: the counter reads 1000 known mallocs as exactly 1000"
        PASS=$((PASS + 1))
    fi
fi

# ── The measured program: patterns/err-default, the idiom the standard teaches
#    (idiom rule 9) and the one 127.49 measured. ──
cat > errdef.tk <<'TKEOF'
m=main;
i=io:std.io;
i=env:std.env;
t=$myerr{$bad:bool};
f=chk(x:i64):i64!$myerr{
  if(x%7==0){<$myerr{$bad:true}};
  <x*2
};
f=pat(x:i64):i64{
  <mt chk(x){$ok:v v;$err:e 0-1}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i)
  };
  io.println("acc=\(acc)");
  <0
};
TKEOF

if ! "${TKC}" --allow-all --diag-json -o errdef errdef.tk >/dev/null 2>&1 || [ ! -x errdef ]; then
    echo "  FAIL: err-default did not compile"
    FAIL=$((FAIL + 1))
else
    # The claim is that the allocation count no longer depends on N at all.
    # One raise vs 142,857 raises vs 1,428,571 raises: the same count.
    a="$(count ./errdef 1000)"
    b="$(count ./errdef 1000000)"
    c="$(count ./errdef 10000000)"
    ma="$(echo "${a}" | cut -d' ' -f1)"; ba="$(echo "${a}" | cut -d' ' -f3)"
    mb="$(echo "${b}" | cut -d' ' -f1)"; bb="$(echo "${b}" | cut -d' ' -f3)"
    mc="$(echo "${c}" | cut -d' ' -f1)"; bc="$(echo "${c}" | cut -d' ' -f3)"

    if [ -z "${ma}" ] || [ -z "${mb}" ] || [ -z "${mc}" ]; then
        echo "  FAIL: no allocation reading for err-default"
        FAIL=$((FAIL + 1))
    elif [ "${ma}" -ne "${mb}" ] || [ "${mb}" -ne "${mc}" ]; then
        echo "  FAIL: the allocation count still grows with the error count:"
        echo "        N=1e3 -> ${ma} mallocs; N=1e6 -> ${mb}; N=1e7 -> ${mc}"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: allocations are constant in N (${ma} at N=1e3, 1e6 and 1e7)"
        PASS=$((PASS + 1))
    fi

    # And the bytes.  1,428,571 raises used to be 22.8 MB of unfreed boxes;
    # the whole program must now stay far under a megabyte.
    if [ -n "${bc}" ] && [ "${bc}" -lt 1000000 ]; then
        echo "  PASS: total bytes malloc'd at N=1e7 is ${bc} (< 1 MB; was 16 bytes per raise)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: total bytes malloc'd at N=1e7 is ${bc:-<none>}, expected < 1 MB"
        FAIL=$((FAIL + 1))
    fi

    # ── 127.49's actual comparison, and the reason it was a P0: the idiom the
    #    standard teaches (`mt` over the error union) against the precondition
    #    guard that does the same thing.  They were 9,143,057 allocations
    #    versus 199 for identical behaviour.  Assert the behaviour is identical
    #    (which is what makes the allocation comparison meaningful) and that
    #    the allocation counts now match — no magic constant on either side. ──
    cat > guard.tk <<'GDEOF'
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64):i64{
  if(x%7==0){<0-1};
  <x*2
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i)
  };
  io.println("acc=\(acc)");
  <0
};
GDEOF
    if ! "${TKC}" --allow-all --diag-json -o guard guard.tk >/dev/null 2>&1 || [ ! -x guard ]; then
        echo "  FAIL: the guard-form control did not compile"
        FAIL=$((FAIL + 1))
    else
        e_out="$(PAT_N=1000000 ./errdef 2>/dev/null)"
        g_out="$(PAT_N=1000000 ./guard 2>/dev/null)"
        if [ -n "${e_out}" ] && [ "${e_out}" = "${g_out}" ]; then
            echo "  PASS: the mt form and the guard form agree (${e_out})"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: the mt form and the guard form disagree: [${e_out}] vs [${g_out}]"
            FAIL=$((FAIL + 1))
        fi

        gm="$(count ./guard 1000000 | cut -d' ' -f1)"
        em="$(count ./errdef 1000000 | cut -d' ' -f1)"
        # The mt form must no longer pay an allocation premium for the idiom.
        # Allow the one buffer the error channel legitimately owns.
        if [ -n "${gm}" ] && [ -n "${em}" ] && [ "$((em - gm))" -le 2 ] && [ "$((em - gm))" -ge -2 ]; then
            echo "  PASS: mt form allocates ${em} vs the guard form's ${gm} (was 9,143,057 vs 199)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: mt form allocates ${em:-<none>} vs the guard form's ${gm:-<none>}"
            FAIL=$((FAIL + 1))
        fi
    fi
fi

fi  # counter built
fi  # Darwin

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
