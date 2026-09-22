#!/usr/bin/env bash
# T006_builtin_table_matches_the_c.sh — the compiler's builtin table is the
# arity every std call is judged against, and half of it was hand-maintained.
#
# 136.1 made `g_stdlib_decls` load-bearing: stdlib_glue_arity() answers out of
# it, and types.c rejects any std call whose argument count disagrees.  That
# table has two halves — the generated one (src/stdlib_decls_gen.h) and a
# hand-written one in src/llvm.c — and gen_stdlib_decls.py EXCLUDES from
# generation anything already declared by hand (load_manual_decls()).  So a
# wrong hand-written entry is never regenerated, never corrected, and never
# noticed.
#
# Two were wrong.  src/stdlib/os.c defines
#
#     int64_t tk_os_read (int64_t fd, int64_t count)
#     int64_t tk_os_write(int64_t fd, int64_t data)
#
# with two parameters each, and llvm.c hand-declared both as `(i64, i64, i64)`.
# `stdlib/os.tki` documents two.  So the interface and the implementation
# AGREED, and the call they both describe was rejected anyway:
#
#     error[E4026]: wrong number of arguments for 'std.os.read':
#                   the implementation takes 3 arguments, the call passes 2
#
# This is the exact inverse of the failure 136.6 catalogues.  Everywhere else
# a drifted `.tki` over-promises and the compiler catches the call; here the
# compiler's own record over-promises and rejects correct code, and no
# document in the tree is wrong.  Nothing could have found it by reading the
# interfaces, which is why the fix is a gate over the two halves of the table
# (scripts/check_tki_coverage.py, c_definition_arities vs declared_arities)
# rather than a correction to two lines.
#
# The correction itself is a DELETION: the hand-written rows are removed so
# gen_stdlib_decls.py derives both from os.c.  Correcting them by hand would
# leave in place the thing that broke.
#
# Story: 137.12
#
# NOTE ON WHAT THIS DOES NOT ASSERT.  It does not check `.tki` parameter
# TYPES.  src/types.c:1434 (`if (is_std) return;`) declines to enforce a
# hand-written std interface against calls at all, so only arity is live.
# T007 covers the interface side.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_builtin_table_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "T006: the builtin table agrees with the C it describes"
echo "--------------------------------------"

# check_case NAME EXPECTED_RC EXPECTED_GREP SOURCE
check_case() {
    local name="$1" exp_rc="$2" pattern="$3" src="$4"
    printf '%s\n' "${src}" > case.tk
    local out rc=0
    out="$("${TKC}" --check --diag-json case.tk 2>&1)" || rc=$?
    if [ "${rc}" != "${exp_rc}" ]; then
        echo "  FAIL: ${name}: expected exit ${exp_rc}, got ${rc}"
        echo "${out}" | sed 's/^/      /' | head -5
        FAIL=$((FAIL + 1))
        return
    fi
    if [ -n "${pattern}" ] && ! printf '%s' "${out}" | grep -q "${pattern}"; then
        echo "  FAIL: ${name}: diagnostics did not match [${pattern}]"
        echo "${out}" | sed 's/^/      /' | head -5
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ── 1. The defect: a call matching BOTH os.tki and os.c was rejected. ──
check_case "os.read(fd;count) compiles — two arguments, as os.c defines it" 0 "" '
m=t;
i=o:std.os;
f=main():i64{
  let s=o.read(0;16);
  <0
};'
check_case "os.write(fd;data) compiles — two arguments, as os.c defines it" 0 "" '
m=t;
i=o:std.os;
f=main():i64{
  let n=o.write(1;"hi");
  <0
};'

# ── 2. The correction is not a widening: the three-argument form the stale
#      declaration described must now be the error. ──
check_case "os.read with three arguments is an arity error" 1 \
  '"error_code":"E4026"' '
m=t;
i=o:std.os;
f=main():i64{
  let s=o.read(0;16;0);
  <0
};'
check_case "E4026 names os.read at its real arity of 2" 1 \
  "std.os.read.: the implementation takes 2 arguments, the call passes 3" '
m=t;
i=o:std.os;
f=main():i64{
  let s=o.read(0;16;0);
  <0
};'

# ── 3. Narrowing guards: the neighbouring hand-written os rows are correct
#      and must keep their arities. tk_os_open really does take three and
#      tk_os_close really does take one. ──
check_case "os.open keeps its three-argument signature" 0 "" '
m=t;
i=o:std.os;
f=main():i64{
  let fd=o.open("/tmp/x";0;0);
  <0
};'
check_case "os.close keeps its one-argument signature" 0 "" '
m=t;
i=o:std.os;
f=main():i64{
  let r=o.close(0);
  <0
};'
check_case "os.open with two arguments is still an arity error" 1 \
  '"error_code":"E4026"' '
m=t;
i=o:std.os;
f=main():i64{
  let fd=o.open("/tmp/x";0);
  <0
};'

# ── 4. The gate that would have caught it, and the reason it is a gate: the
#      two halves of g_stdlib_decls must describe the same C. ──
echo "  ---- scripts/check_tki_coverage.py: g_stdlib_decls vs the C ----"
GATE_OUT="$(cd "${REPO_ROOT}" && python3 scripts/check_tki_coverage.py 2>&1)"
if printf '%s' "${GATE_OUT}" | grep -q "g_stdlib_decls entries disagreeing with the C definition: 0"; then
    echo "  PASS: no g_stdlib_decls entry disagrees with its C definition"
    PASS=$((PASS + 1))
else
    echo "  FAIL: the builtin table still disagrees with the C sources"
    printf '%s' "${GATE_OUT}" | grep -A6 "g_stdlib_decls disagrees" | sed 's/^/      /'
    FAIL=$((FAIL + 1))
fi

# ── 5. The interface surface 137.12 was filed for: names that are reachable
#      from toke source and were absent from their .tki. Each must compile
#      (they always did) AND now be declared (scripts/gen_tki.py --check is
#      the gate on the second half; this asserts the first). ──
check_case "str.padright compiles at its real arity" 0 "" '
m=t;
i=s:std.str;
f=main():i64{
  let a=s.padright("x";5;32);
  <0
};'
check_case "file.listglob compiles" 0 "" '
m=t;
i=f:std.file;
f=main():i64{
  let a=f.listglob("/tmp/*.txt");
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
