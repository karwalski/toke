#!/usr/bin/env bash
# C1351_record_len_not_array_header.sh -- no array-length read is applied to
# a record block (story 127.355).
#
# THE DEFECT.  A toke record is a bare malloc(nfields * 8) with no header
# (src/llvm.c struct_lit), so its word [-1] is 8 bytes before the allocation.
# The 127.355 audit of every length read in src/ and src/stdlib found two ways
# a record reached one:
#   (a) `r.len` on a record that DECLARES a field named len.  NODE_FIELD_EXPR
#       took `.len` as the array length before it looked at struct fields, so
#       it loaded r[-1] (printed 1, or -1 after `+1`) instead of the field;
#   (b) a built-in receiver method on a record: `r.len()` read r[-1] the same
#       way, and `r.contains(x)` / `r.slice(a;b)` / `r.indexof(x)` were lowered
#       to str glue that walks the record's slots as a NUL-terminated string.
#       All passed --check (a scalar receiver was already E4036, 127.229).
# Now (a) is a field read, and (b) is E4036 at --check with no `fix` (which
# field, if any, was meant is not decidable).  Arrays keep `.len` / `.len()`.
#
# WHAT IS ASSERTED:
#   1. toke, IR linked against ASan-built stdlib objects: `r.len`, `r.len+1`,
#      a loop bounded by `r.len`, a nested `o.in.len`, and an array's
#      `.len`/`.len()` beside them -- exact output.  (The program's own loads
#      are not ASan-instrumented: tkc's IR carries no sanitize_address
#      attribute.  The wrong values are the evidence: main reads the malloc
#      chunk word before the record.)
#   2. --check: `r.len()`, `r.contains(0)`, `r.slice(0;1)`, `r.indexof(1)` on a
#      record are E4036 with no `fix`; the same four on an array / str are
#      rc 0 (control).
#
# Negative control: TKC=<main tkc>, main aace8185: 2 passed, 5 failed --
# part 1 prints len=71 plus=72 loop=71 nested=77 (the word before each
# record), and all four part-2 record calls are rc 0.  This branch: 7 passed.
#
# Story: 127.355
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${REPO_ROOT}/src/stdlib"
export TKC_STDLIB_DIR="${S}"
# 127.439: clang, not cc: this harness links tkc's .ll directly, and on Linux
# cc is gcc, which reads a .ll as a linker script ("p.ll:1: syntax error").
CC="${CC:-clang}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1351_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1351: no array-length read on a record (127.355)"
echo "----------------------------------------------------------------------"

# ── 1. toke, under ASan ──────────────────────────────────────────────────────
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects io -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "C1351: ${PASS} passed, ${FAIL} failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
cat > p.tk <<'TOKE'
m=main;
i=io:std.io;
t=R{x:i64;len:i64};
t=O{tag:i64;in:R};
f=main():i64{
  let r=R{x:3;len:42};
  io.println("len=\(r.len)");
  let q=r.len+1;
  io.println("plus=\(q)");
  let n=mut.0;
  lp(let i=0;i<r.len;i=i+1){n=n+1};
  io.println("loop=\(n)");
  let o=O{tag:1;in:R{x:0;len:7}};
  io.println("nested=\(o.in.len)");
  let a=@(5;6;7);
  io.println("arr=\(a.len) \(a.len())");
  <0
};
TOKE
if ! "${TKC}" --emit-llvm p.tk -o p.ll > p.log 2>&1; then
    fail "p.tk did not compile: $(head -c 300 p.log)"
elif ! "${CC}" ${SAN} -g -o p p.ll "${LS[@]}" -lpthread -lm -lz > p.cc 2>&1; then
    fail "p.ll did not link: $(grep -m1 -E 'error|undefined' p.cc | head -c 200)"
else
    ./p > p.out 2>&1; rc=$?
    [ "${rc}" -ne 0 ] && fail "p: exited ${rc}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' p.out | head -c 300)"
    for want in "len=42" "plus=43" "loop=42" "nested=7" "arr=3 3"; do
        if [ "${rc}" -eq 0 ] && grep -a -q -x -F -- "${want}" p.out; then pass "toke: ${want}"
        else fail "toke: expected '${want}' (exit ${rc}), got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"; fi
    done
fi

# ── 2. receiver methods on a record are E4036 at --check ─────────────────────
H='m=main;i=io:std.io;t=S{x:i64;y:i64};'
rec_bad=0; rec_n=0
for m in 'len()' 'contains(0)' 'slice(0;1)' 'indexof(1)'; do
    printf '%s\n' "${H}f=main():i64{let r=S{x:1;y:2};let z=r.${m};<0};" > r.tk
    rec_n=$((rec_n + 1))
    if "${TKC}" --check r.tk > r.log 2>&1; then rec_bad=$((rec_bad + 1)); echo "        r.${m}: rc 0"
    elif ! grep -q '"error_code":"E4036"' r.log || grep -q '"fix"' r.log; then
        rec_bad=$((rec_bad + 1)); echo "        r.${m}: $(head -c 200 r.log)"
    fi
done
if [ "${rec_bad}" -eq 0 ]; then pass "record receiver: ${rec_n}/${rec_n} methods are E4036, no fix"
else fail "record receiver: ${rec_bad}/${rec_n} not E4036 (or carry a fix)"; fi
ctl_bad=0
for body in 'let a=@(1;2);let z=a.len();' 'let a=@(1;2);let z=a.contains(1);' \
            'let a=@(1;2);let z=a.slice(0;1);' 'let s="ab";let z=s.indexof("b");'; do
    printf '%s\n' "${H}f=main():i64{${body}<0};" > c.tk
    "${TKC}" --check c.tk > c.log 2>&1 || { ctl_bad=$((ctl_bad + 1)); echo "        control ${body}: $(head -c 200 c.log)"; }
done
if [ "${ctl_bad}" -eq 0 ]; then pass "control: the same methods on an array / str check clean"
else fail "control: ${ctl_bad} array/str receiver call(s) rejected"; fi

echo "----------------------------------------------------------------------"
echo "C1351: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
