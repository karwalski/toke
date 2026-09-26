#!/usr/bin/env bash
# C111_tkir_emit_read_roundtrip.sh — a .tkir the compiler writes can be read
# back (story 127.211).
#
# THE DEFECT.  .tkir was write-only.  The encoder wrote the magic as the
# bytes "TKIR" and the reader compared a little-endian u32 with 0x544B4952
# (the bytes read big-endian), so every real file failed "invalid magic".
# Behind that: the code section had no per-function framing, instructions
# used a different operand layout per opcode, array/fn-pointer type entries
# lacked the u16 the reader expects, and every struct field was recorded as
# i64 (tkir.c read the FIELD node's type from children[1]; it is children[0]).
# test_tkir_reader only decoded hand-built buffers, so it passed throughout.
#
# WHAT THIS DRIVES.  (1) test/tkir/test_tkir_reader with the compiler path:
# its emit->read case compiles a module with a five-field struct and two
# functions, reads the file with tkir_read and checks field kinds, the array
# element type, framed code ending in RET, the CALL target, the string datum
# and the export.  (2) every example under examples/ that --emit-tkir accepts
# must read back.
#
# Negative control: put fld->children[1] back in enc_prepass_types —
# "Assertion failed: (mod.types[pt->field_type_idxs[k]].kind == want[k])";
# put TKIR_MAGIC back to 0x544B4952u — test_magic_is_ascii aborts.
#
# Story: 127.211

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c111_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C111: .tkir emit -> read round trip"
echo "-----------------------------------"

if "${CC}" -std=c99 -D_GNU_SOURCE -I"${REPO_ROOT}/src" -o "${WORK}/rd" \
        "${REPO_ROOT}/test/tkir/test_tkir_reader.c" \
        "${REPO_ROOT}/src/tkir.c" "${REPO_ROOT}/src/diag.c" >"${WORK}/cc.log" 2>&1; then
    pass "reader test builds"
else
    fail "reader test does not build: $(head -c 400 "${WORK}/cc.log")"
    echo ""; echo "C111: ${PASS} passed, ${FAIL} failed"; exit 1
fi

if "${WORK}/rd" "${TKC}" >"${WORK}/rd.log" 2>&1 &&
   /usr/bin/grep -a -q "test_emit_read_roundtrip ... PASS" "${WORK}/rd.log"; then
    pass "unit tests incl. emit->read: $(tail -1 "${WORK}/rd.log")"
else
    fail "reader tests: $(tail -3 "${WORK}/rd.log")"
fi

# A tiny reader driver for the sweep: exit 0 iff tkir_read accepts the file.
cat > "${WORK}/chk.c" <<'C'
#include <stdio.h>
#include "tkir.h"
int main(int argc, char **argv) {
    TkirModule m; char err[256] = {0};
    if (argc < 2 || tkir_read(argv[1], &m, err, (int)sizeof err) != 0) {
        printf("%s\n", err); return 1;
    }
    tkir_module_free(&m); return 0;
}
C
"${CC}" -std=c99 -D_GNU_SOURCE -I"${REPO_ROOT}/src" -o "${WORK}/chk" "${WORK}/chk.c" \
    "${REPO_ROOT}/src/tkir.c" "${REPO_ROOT}/src/diag.c" >/dev/null 2>&1 || fail "chk does not build"

n=0; bad=0
while IFS= read -r f; do
    "${TKC}" --emit-tkir "${f}" --out "${WORK}/x.tkir" >/dev/null 2>&1 || continue
    n=$((n + 1))
    if ! msg="$("${WORK}/chk" "${WORK}/x.tkir")"; then
        bad=$((bad + 1)); echo "    unreadable: ${f#${REPO_ROOT}/}: ${msg}"
    fi
    rm -f "${WORK}/x.tkir"
done < <(find "${REPO_ROOT}/examples" -name '*.tk' -o -name '*.toke' | sort)
if [ "${n}" -gt 0 ] && [ "${bad}" -eq 0 ]; then
    pass "all ${n} emitted example .tkir files read back"
else
    fail "${bad} of ${n} emitted example .tkir files unreadable"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
