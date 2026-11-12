#!/usr/bin/env bash
# C1281_read_tkir.sh -- `tkc --read-tkir <f.tkir>` prints a summary of a
# .tkir file, and refuses one it cannot decode with E9021 (story 127.345).
#
# THE DEFECT.  Story 76.1.6b's row and the tkir_dump comment in src/tkir.h
# promised a --read-tkir flag; src/main.c parsed none and nothing called
# tkir_dump, so the reader's dump existed only for a unit test.
#
# NOW --read-tkir decodes each input with the reader's validation, prints
# tkir_dump (types, functions with opcode histograms, data, imports, exports)
# and the sizes of the format-1.2 TEXT/AST sections, and exits 0.  It
# compiles nothing and needs no stdlib.
#
# CASES.
#   1. A good format-1.2 file: exit 0; the header names v1.2; the function
#      table lists main with its params; the data and export tables are
#      present; TEXT and AST sizes are printed; nothing is written beside it.
#   2. Two inputs: each summary is headed `== <path> ==`.
#   3. A file without the AST section (the TEXT/AST section entries cut and
#      the section count lowered, i.e. a 1.1-shaped file): exit 0 and
#      `AST=absent` with the note that --from-tkir needs 1.2.
#   4. A missing file, a file with a bad magic number and a truncated file:
#      exit 1 and an E9021 JSON diagnostic naming the problem, no signal.
#   5. `tkc --help` lists --read-tkir.
#
# Negative control (run 2026-11-10): with the `--read-tkir` branch removed
# from src/main.c the flag is ignored and the .tkir is compiled as source,
# so every case but 5 FAILS.
#
# Story: 127.345

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1281_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1281: --read-tkir prints a .tkir summary; E9021 on a bad file"
echo "---------------------------------------------------------------"

cat > "${WORK}/ok.tk" <<'EOF'
m=ok;
f=add(a:i64;b:i64):i64{<a+b};
f=main():i64{<add(40;2)};
EOF
if ! "${TKC}" "${WORK}/ok.tk" --emit-tkir --out "${WORK}/ok.tkir" >"${WORK}/emit.log" 2>&1; then
    fail "cannot emit the base .tkir: $(head -c 300 "${WORK}/emit.log")"
    echo ""; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# ── 1. a good file ──────────────────────────────────────────────────────────
before="$(ls "${WORK}" | sort)"
"${TKC}" --read-tkir "${WORK}/ok.tkir" >"${WORK}/ok.out" 2>"${WORK}/ok.err"
rc=$?
after="$(ls "${WORK}" | sort | grep -v -e '^ok\.out$' -e '^ok\.err$')"
if [ "${rc}" -eq 0 ] &&
   grep -q '^TKIR Module v1\.2 ' "${WORK}/ok.out" &&
   grep -q "^  \[[0-9]*\] 'add' .*params=2 " "${WORK}/ok.out" &&
   grep -q "^  \[[0-9]*\] 'main' .*params=0 " "${WORK}/ok.out" &&
   grep -q '^Data: [0-9]' "${WORK}/ok.out" &&
   grep -q "^  \[[0-9]*\] 'main'  kind=func" "${WORK}/ok.out" &&
   grep -Eq '^Program sections: TEXT=[1-9][0-9]* bytes AST=[1-9][0-9]* bytes$' "${WORK}/ok.out" &&
   [ ! -s "${WORK}/ok.err" ] && [ "${before}" = "${after}" ]; then
    pass "good 1.2 file: header, functions, data, exports, TEXT/AST sizes; nothing written"
else
    fail "good file: rc=${rc}: $(head -c 400 "${WORK}/ok.out") $(head -c 200 "${WORK}/ok.err")"
fi

# ── 2. two inputs ───────────────────────────────────────────────────────────
cp "${WORK}/ok.tkir" "${WORK}/two.tkir"
"${TKC}" --read-tkir "${WORK}/ok.tkir" "${WORK}/two.tkir" >"${WORK}/two.out" 2>&1
rc=$?
if [ "${rc}" -eq 0 ] &&
   [ "$(grep -c '^== .*\.tkir ==$' "${WORK}/two.out")" -eq 2 ] &&
   [ "$(grep -c '^TKIR Module' "${WORK}/two.out")" -eq 2 ]; then
    pass "two inputs: two headed summaries"
else
    fail "two inputs: rc=${rc}: $(head -c 300 "${WORK}/two.out")"
fi

# ── 3. no AST section (1.1-shaped) ──────────────────────────────────────────
# Header: magic(4) vmaj(1) vmin(1) flags(1) count(u16); then sections of
# tag(u8) len(u32) body.  Keep the first six (TYPE..EXPORT), drop TEXT/AST.
if command -v python3 >/dev/null 2>&1; then
    python3 - "${WORK}/ok.tkir" "${WORK}/noast.tkir" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
pos, out = 9, []
n = struct.unpack_from('<H', b, 7)[0]
for _ in range(n):
    tag, ln = b[pos], struct.unpack_from('<I', b, pos + 1)[0]
    if tag not in (0x08, 0x09):
        out.append(b[pos:pos + 5 + ln])
    pos += 5 + ln
hdr = b[:5] + bytes([1]) + b[6:7] + struct.pack('<H', len(out))
open(sys.argv[2], 'wb').write(hdr + b''.join(out))
PY
    "${TKC}" --read-tkir "${WORK}/noast.tkir" >"${WORK}/noast.out" 2>&1
    rc=$?
    if [ "${rc}" -eq 0 ] &&
       grep -q '^TKIR Module v1\.1 ' "${WORK}/noast.out" &&
       grep -q 'TEXT=absent AST=absent  (--from-tkir needs both' "${WORK}/noast.out"; then
        pass "1.1-shaped file: summarised, TEXT/AST reported absent"
    else
        fail "1.1-shaped file: rc=${rc}: $(tail -c 300 "${WORK}/noast.out")"
    fi
else
    echo "  SKIP: python3 not available for the 1.1-shaped case"
fi

# ── 4. unreadable files ─────────────────────────────────────────────────────
printf 'XXXXXXXXXXXXXXXX' > "${WORK}/magic.tkir"
head -c 20 "${WORK}/ok.tkir" > "${WORK}/trunc.tkir"
check_bad() {   # <label> <file> <message fragment>
    "${TKC}" --read-tkir "$2" >"${WORK}/bad.out" 2>&1
    local rc=$?
    if [ "${rc}" -eq 1 ] &&
       grep -q '"error_code":"E9021"' "${WORK}/bad.out" &&
       grep -q "$3" "${WORK}/bad.out"; then
        pass "$1 -> E9021, exit 1"
    else
        fail "$1: rc=${rc}: $(head -c 300 "${WORK}/bad.out")"
    fi
}
check_bad "missing file" "${WORK}/nope.tkir" "cannot open"
check_bad "bad magic"    "${WORK}/magic.tkir" "failed to read .tkir file"
check_bad "truncated"    "${WORK}/trunc.tkir" "failed to read .tkir file"

# ── 5. --help ───────────────────────────────────────────────────────────────
if "${TKC}" --help 2>/dev/null | grep -q '^  --read-tkir  '; then
    pass "--help lists --read-tkir"
else
    fail "--help does not list --read-tkir"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
