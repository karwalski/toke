#!/usr/bin/env bash
# C738_migrate_buffer_grows.sh — `tkc --migrate`'s token-rewrite buffer grows
# with its output instead of assuming a fixed expansion (127.286).
#
# The step-3 buffer in migrate_core() was a fixed clen*3+2048 bytes and was
# written without a bounds check.  A token can expand past 3x: 127.251's
# `Vec` → `$vechandle` is 3 → 10 bytes, and an unmatched legacy `[` the
# prepass leaves alone becomes `.get(`, 1 → 5.  4000 of the latter wrote
# ~6 KB past the end of the heap block.  A plain build printed the right
# output anyway, so the check runs the compiler under a guard allocator
# (libgmalloc on macOS, MALLOC_CHECK_ on glibc); under the ASan gate
# (make test-asan-compiler) ASan itself reports the overrun.
#
#   1. a file dense in `Vec` (3000 fields) migrates every one to $vechandle,
#      leaves no `Vec`, and the result passes --check;
#  1b. 4000 x `Vec;` (2.75x, the densest Vec text) migrates byte for byte;
#   2. 4000 unmatched `[` migrate to 4000 `.get(` without a heap overrun.
#
# Negative control (recorded in 127.286): the compiler at main 8f2dcce3 under
# libgmalloc exits 139 on case 2; under ASan it reports heap-buffer-overflow
# in migrate_core.
#
# Story: 127.286

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc not found at ${TKC}"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/c738.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; [ -n "${2:-}" ] && echo "    $2"; FAIL=$((FAIL + 1)); }

# Run tkc under a guard allocator when one works with this binary.  An ASan
# build refuses libgmalloc (it is killed at start-up), so probe first and fall
# back to a plain run, where ASan does the checking.
GUARD=()
if [ "$(uname)" = Darwin ] && [ -e /usr/lib/libgmalloc.dylib ]; then
    if bash -c 'DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib "$1" --version; exit $?' _ "${TKC}" >/dev/null 2>&1; then
        GUARD=(env DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib)
    fi
else
    GUARD=(env MALLOC_PERTURB_=85 MALLOC_CHECK_=3)
fi
migrate() { ${GUARD[@]+"${GUARD[@]}"} "${TKC}" --migrate "$1" 2>"${WORK}/err"; }

# ── 1. dense in Vec ─────────────────────────────────────────────────────
{
    printf 'm=t;\ni=v:std.vec;\nt=$h{'
    for k in $(seq 0 2999); do
        [ "$k" -gt 0 ] && printf ';'
        printf 'a%d:Vec' "$k"
    done
    printf '};\nf=main():i64{<0};\n'
} > "${WORK}/vec.tk"

if migrate "${WORK}/vec.tk" > "${WORK}/vec.out"; then
    n="$(grep -o '\$vechandle' "${WORK}/vec.out" | wc -l | tr -d ' ')"
    left="$(grep -c 'Vec' "${WORK}/vec.out" | tr -d ' ')"
    if [ "$n" -eq 3000 ] && [ "$left" -eq 0 ]; then
        pass "1-vec-dense: 3000 Vec -> \$vechandle"
    else
        fail "1-vec-dense" "\$vechandle=${n} (want 3000), lines still naming Vec=${left} (want 0)"
    fi
    if "${TKC}" --check "${WORK}/vec.out" >/dev/null 2>&1; then
        pass "1-vec-dense-checks"
    else
        fail "1-vec-dense-checks" "$("${TKC}" --check "${WORK}/vec.out" 2>&1 | head -c 300)"
    fi
else
    fail "1-vec-dense: --migrate exited $?" "$(tail -3 "${WORK}/err")"
fi

# ── 1b. as dense as Vec gets: `Vec;` x4000, 4 bytes → 11 (2.75x) ──────
# Not a valid program (a parameter list of bare types); --migrate is a text
# rewriter and only needs it to lex, and the output is compared byte for byte.
vecs() {  # $1 = spelling, $2 = count: "<s>;<s>;...;<s>"
    for k in $(seq 1 "$2"); do [ "$k" -gt 1 ] && printf ';'; printf '%s' "$1"; done
}
{ printf 'm=t;\ni=v:std.vec;\nf=g(a:'; vecs Vec 4000; printf '):i64{<0};\n'; } > "${WORK}/vd.tk"
{ printf 'm=t;\ni=v:std.vec;\nf=g(a:'; vecs '$vechandle' 4000; printf '):i64{<0};\n'; } > "${WORK}/vd.want"
if migrate "${WORK}/vd.tk" > "${WORK}/vd.out"; then
    if cmp -s "${WORK}/vd.want" "${WORK}/vd.out"; then
        pass "1b-vec-densest: 4000 x 'Vec;' -> '\$vechandle;', byte for byte"
    else
        fail "1b-vec-densest" "output differs from expected ($(wc -c < "${WORK}/vd.out" | tr -d ' ') bytes, want $(wc -c < "${WORK}/vd.want" | tr -d ' '))"
    fi
else
    fail "1b-vec-densest: --migrate exited $?" "$(tail -3 "${WORK}/err")"
fi

# ── 2. unmatched '[' run: 1 byte → 5 ────────────────────────────────────
{
    printf 'm=t;\nf=g():i64{let x=1;<x};\n'
    head -c 4000 /dev/zero | tr '\0' '['
    printf '\n'
} > "${WORK}/br.tk"

migrate "${WORK}/br.tk" > "${WORK}/br.out"
rc=$?
if [ "$rc" -eq 0 ]; then
    n="$(grep -o '\.get(' "${WORK}/br.out" | wc -l | tr -d ' ')"
    if [ "$n" -eq 4000 ]; then
        pass "2-bracket-run: 4000 [ -> .get( with no overrun"
    else
        fail "2-bracket-run" ".get( count ${n}, want 4000"
    fi
else
    fail "2-bracket-run: --migrate exited ${rc}" "$(grep -m2 -E 'ERROR|SUMMARY' "${WORK}/err")"
fi

echo "C738: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
