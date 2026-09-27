#!/usr/bin/env bash
# C094_tkir_wide_structs.sh — --emit-tkir records every field of a struct
# wider than 16 fields.
#
# Story: 127.207 (the .tkir half of 127.190).
#
# THE DEFECT.  src/tkir.c sized each struct's field table by TKC_MAX_PARAMS
# (16) and stopped appending at 16 without a word, so a 40-field struct was
# written to the .tkir with a field count of 16.  127.190 lifted the same
# limit from llvm.c; this is the encoder's copy.
#
# HOW.  The .tkir type section is walked here byte for byte (the layout
# enc_write_type_section writes), not with tkir_read(): the reader does not
# accept the encoder's output at all (its magic check reads "TKIR" as a
# little-endian u32 -- filed separately), so it cannot be the oracle.
#
# Field TYPES are not asserted: the encoder records every field as i64
# because it reads the field name from children[0] and the type from a
# children[1] the parser never builds -- also filed separately.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_tkirwide_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C094: --emit-tkir keeps every field of a wide struct"
echo "----------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Structs of 3, 16, 17 and 40 fields, mixed field types, and a struct-typed
# field so a type reference sits among the fields.
python3 - <<'EOF'
tys = ["i64", "str", "f64", "bool"]
def st(name, n):
    return "t=$%s{%s};" % (name, ";".join("f%d:%s" % (i, tys[i % 4]) for i in range(n)))
src = ["m=t;", st("small", 3), st("sixteen", 16), st("seventeen", 17), st("forty", 40),
       "t=$holder{a:i64;b:$forty;c:str};",
       "f=main():i64{<0};"]
open("wide.tk", "w").write("\n".join(src) + "\n")
EOF

rc=0; "${TKC}" --diag-json --emit-tkir --out wide.tkir wide.tk > emit.log 2>&1 || rc=$?
if [ "${rc}" -ne 0 ] || [ ! -s wide.tkir ]; then
    fail "--emit-tkir on wide.tk: rc=${rc}"; head -3 emit.log | sed 's/^/      /'
    echo ""; echo "C094: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# Print "name count" for every struct in the type section, and "OK" if every
# section's declared length walks exactly to the end of the file.
python3 - wide.tkir > structs.txt <<'EOF'
import struct, sys
b = open(sys.argv[1], "rb").read()
assert b[:4] == b"TKIR", "magic"
pos = 9                     # magic(4) major(1) minor(1) flags(1) nsections(2)
out = []
while pos < len(b):
    sid = b[pos]; (ln,) = struct.unpack_from("<I", b, pos + 1); body = b[pos + 5: pos + 5 + ln]
    if len(body) != ln: print("TRUNCATED"); sys.exit(0)
    if sid == 1:            # TKIR_SEC_TYPE
        p = 0; (cnt,) = struct.unpack_from("<H", body, p); p += 2
        for _ in range(cnt):
            kind = body[p]; p += 1
            if kind == 6:   # TKIR_TY_STRUCT
                (nl,) = struct.unpack_from("<H", body, p); p += 2
                name = body[p:p + nl].decode(); p += nl
                (fc,) = struct.unpack_from("<H", body, p); p += 2 + 2 * fc
                out.append("%s %d" % (name, fc))
        if p != ln: out.append("TYPE-SECTION-LENGTH-MISMATCH %d %d" % (p, ln))
    pos += 5 + ln
print("\n".join(out)); print("OK" if pos == len(b) else "OVERRUN")
EOF

for want in "small 3" "sixteen 16" "seventeen 17" "forty 40" "holder 3"; do
    if grep -qx "${want}" structs.txt; then pass "struct ${want% *}: ${want#* } fields"
    else fail "struct ${want% *}: want ${want#* } fields, got '$(grep "^${want% *} " structs.txt)'"; fi
done
if grep -qx OK structs.txt && ! grep -q MISMATCH structs.txt; then
    pass "section lengths walk exactly to end of file"
else
    fail "section layout: $(tr '\n' ' ' < structs.txt)"
fi

echo ""
echo "C094: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
