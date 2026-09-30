#!/usr/bin/env bash
# C641 -- arena_alloc sizes are size_t end to end; nothing is narrowed to int
# on the way in (127.258).
#
# arena_alloc took an `int`.  types.c passed it a size_t product (truncated
# at the call), and 50-odd call sites multiplied `n * (int)sizeof(T)` in int,
# which overflows before 121.29's guard can see it.  Audit: from source the
# sizes are bounded by the 64 MiB source limit (Token is 20 bytes, so a token
# array is at most ~1.3 GB; a struct's NAME_BUF field table would need ~16.7M
# fields, more than 64 MiB of source can spell), so no call site passes
# INT_MAX from a program.  The --max-* caps reach it: codegen sized its
# registries `cap * (int)sizeof(Slot)`, so `--max-funcs=1000000` (2.6 GB of
# FnSig) went negative -> NULL -> SIGSEGV, and `--max-funcs=2000000` wrapped
# to a ~0.9 GB block indexed as if it held two million entries.
#
# arena_alloc now takes size_t and refuses what a block cannot hold; callers
# multiply in size_t; codegen refuses unallocatable caps with E9010.
#
# Negative control: TKC=<2aa48c88's toke> -- cases 1 and 3 fail (rc=139),
# case 2 fails (rc=0, no diagnostic).  Case 5 fails on 2aa48c88's
# src/arena.c (ARENA_SRC=...): a 4 GiB + 64 request truncated to 64 and was
# granted.
#
# Story: 127.258

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
ARENA_SRC="${ARENA_SRC:-${REPO_ROOT}/src}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
printf 'm=q;f=main():i64{<0};\n' > "${WORK}/p.tk"
# "rc=N codes" for an --emit-llvm run with the given flag
run() {
    "${TKC}" "$1" --emit-llvm "${WORK}/p.tk" -o "${WORK}/p.ll" > "${WORK}/o" 2>&1; rc=$?
    codes=$(grep -o '"error_code":"[A-Z0-9]*"' "${WORK}/o" | sed 's/.*:"\(.*\)"/\1/' | sort -u | tr '\n' ' ')
    names=$(grep -c "$2" "${WORK}/o")
    echo "rc=${rc} ${codes}named=${names}"
}

expect "1 --max-funcs=1000000 (2.6 GB of FnSig) is E9010, not SIGSEGV" \
    "$(run --max-funcs=1000000 max_funcs=1000000)" "rc=2 E9010 named=1"
expect "2 --max-funcs=2000000 (wrapped to a positive int on main) is E9010" \
    "$(run --max-funcs=2000000 max_funcs=2000000)" "rc=2 E9010 named=1"
expect "3 --max-structs=100000000 is E9010, not SIGSEGV" \
    "$(run --max-structs=100000000 max_structs=100000000)" "rc=2 E9010 named=1"
expect "4 a large cap that fits still builds" \
    "$(run --max-funcs=4096 max_funcs)" "rc=0 named=0"

# 5: the allocator itself, past INT_MAX, in size_t.
if command -v cc >/dev/null 2>&1; then
    cat > "${WORK}/a.c" <<'C'
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include "arena.h"
int main(void) {
    Arena *a = arena_init();
    if (!a) return 3;
    size_t big[] = { (size_t)INT_MAX + 1, ((size_t)4 << 30) + 64,
                     (size_t)INT_MAX, SIZE_MAX };
    int granted = 0;
    for (int i = 0; i < 4; i++) if (arena_alloc(a, big[i])) granted++;
    void *ok = arena_alloc(a, 64);
    printf("granted=%d small=%s\n", granted, ok ? "ok" : "NULL");
    arena_free(a);
    return 0;
}
C
    if cc -std=c99 -I"${ARENA_SRC}" -I"${REPO_ROOT}/src" -o "${WORK}/a" "${WORK}/a.c" \
          "${ARENA_SRC}/arena.c" > "${WORK}/cc.log" 2>&1; then
        expect "5 arena_alloc refuses sizes past INT_MAX (4 GiB + 64 is not 64)" \
            "$("${WORK}/a")" "granted=0 small=ok"
    else
        expect "5 arena_alloc test compiles" "$(head -3 "${WORK}/cc.log")" ""
    fi
else
    echo "SKIP 5 no cc"
fi

echo "C641: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
