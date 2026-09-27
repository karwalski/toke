#!/usr/bin/env bash
# C351_tkir_imports_past_default_cap.sh — --emit-tkir writes every import,
# not only the first 32 (story 127.235).
#
# THE DEFECT.  tkir.c's encoder held imports in a fixed
# enc_imports[TKC_MAX_IMPORTS] array (32) and skipped the rest.  --max-imports
# (and tkc.toml's max_imports) raise the compiler's import cap, so a module
# with 40 imports compiled and wrote a .tkir whose import section silently
# listed 32 of them.  The array is now sized from the module's own imports.
#
# WHAT IS ASSERTED.  A module with 40 imports, compiled with
# --max-imports=64 --emit-tkir, reads back through tkir_read with
# import_count 40 and the last import (a39) present; one with 32 still reads
# back with 32 (the boundary the old array allowed).
#
# Negative control (run 2026-09-28): TKC=<main 8f63fc6 build> fails the
# 40-import case with import_count 32 and last symbol a31.
#
# Story: 127.235
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c351_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
echo "C351: --emit-tkir keeps imports past the default cap of 32 (127.235)"
echo "------------------------------------------------------------------------"
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

cat > "${WORK}/rd.c" <<'CEOF'
#include <stdio.h>
#include "tkir.h"
int main(int argc, char **argv) {
    TkirModule m; char err[256] = "";
    if (argc < 2 || tkir_read(argv[1], &m, err, (int)sizeof err) != 0) {
        printf("READ-ERROR %s\n", err); return 1;
    }
    printf("%d|%s\n", m.import_count,
           m.import_count > 0 ? m.imports[m.import_count - 1].name : "");
    tkir_module_free(&m);
    return 0;
}
CEOF
if ! "${CC}" -std=c99 -D_GNU_SOURCE -I"${REPO_ROOT}/src" -o "${WORK}/rd" "${WORK}/rd.c" \
        "${REPO_ROOT}/src/tkir.c" "${REPO_ROOT}/src/diag.c" >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: reader does not build: $(head -c 400 "${WORK}/cc.log")"
    echo ""; echo "C351: ${PASS} passed, 1 failed"; exit 1
fi

mk() {  # mk N FILE: a module with N aliases of std.str
    { echo "m=t;"; i=0; while [ "$i" -lt "$1" ]; do echo "i=a$i:std.str;"; i=$((i + 1)); done
      echo "f=main():i64{<0};"; } > "$2"
}

for n in 32 40; do
    mk "$n" "${WORK}/imp$n.tk"
    "${TKC}" --max-imports=64 --emit-tkir -o "${WORK}/imp$n.tkir" "${WORK}/imp$n.tk" >"${WORK}/c$n.log" 2>&1; rc=$?
    expect "$n imports: --emit-tkir exits 0" "${rc}" "0"
    expect "$n imports: read back count|last symbol" "$("${WORK}/rd" "${WORK}/imp$n.tkir")" "$n|a$((n - 1))"
done

echo ""
echo "C351: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
