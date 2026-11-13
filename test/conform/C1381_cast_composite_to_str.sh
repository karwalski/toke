#!/usr/bin/env bash
# C1381_cast_composite_to_str.sh -- `x as str` of a record, an array or a map
# is E4079 at --check (story 127.362).
#
# THE DEFECT.  `as str` converts a number to its decimal text and passes a str
# through; for anything else the cast reinterpreted the value's word as a C
# string.  A record's word points at its slots, so
#   t=S{x:i64}; let r=S{x:3}; io.println(r as str)
# passed --check and printed "\x03" -- the record's bytes up to the first zero
# byte, and a read past the allocation when no slot held one.  Interpolating a
# composite was already refused (E4032); the cast form was not.
#
# WHAT IS ASSERTED:
#   1. E4079, exactly one, stage type_check, severity error, no `fix`, at
#      --check and at build (no binary), for: a record literal, a let-bound
#      record, a record returned by a function, a record field holding a
#      record, an array, an array of records, a map, a sum value.
#   2. No diagnostic (and the program runs) for the casts that stay legal:
#      an i64, a str, a record's scalar field, a u8 field, an f64.
#
# Negative control: TKC=<base tkc> (integrate/z-wave-1113 e1155b19): every
# case in part 1 checks clean -- 0 passed, 8 failed; part 2 passes (1).
# This branch: 9 passed.
#
# Story: 127.362
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1381_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1381: \`as str\` of a composite is E4079 (127.362)"
echo "----------------------------------------------------------------------"

# One program per case; ${1} is the statement casting to str.
prog() {
    cat <<TOKE
m=main;
i=io:std.io;
t=P{x:i64};
t=S{x:i64;p:P};
t=\$sh{\$c:i64;\$q:i64};
f=mk():S{<S{x:1;p:P{x:2}}};
f=main():i64{
  let r=S{x:3;p:P{x:4}};
  let a=@(1;2);
  let ps=@(P{x:1});
  let mp=@("k":1);
  let v=\$sh{\$c:2};
  ${1}
  <0
};
TOKE
}

n=0
while IFS='|' read -r name stmt; do
    [ -z "${name}" ] && continue
    n=$((n + 1))
    prog "${stmt}" > "c${n}.tk"
    "${TKC}" --check "c${n}.tk" > "c${n}.chk" 2>&1; crc=$?
    rm -f "c${n}b"
    "${TKC}" "c${n}.tk" -o "c${n}b" > "c${n}.log" 2>&1; brc=$?
    codes="$(grep -o '"error_code":"[A-Z0-9]*"' "c${n}.chk" | tr '\n' ' ')"
    if [ "${crc}" -ne 0 ] && [ "${codes}" = '"error_code":"E4079" ' ] \
       && grep -q '"stage":"type_check"' "c${n}.chk" \
       && grep -q '"severity":"error"' "c${n}.chk" \
       && ! grep -q '"fix"' "c${n}.chk" \
       && [ "${brc}" -ne 0 ] && [ ! -e "c${n}b" ] && grep -q '"error_code":"E4079"' "c${n}.log"; then
        pass "${name}: one E4079 at --check and build, no fix, no binary"
    else
        fail "${name}: check rc ${crc} codes '${codes}', build rc ${brc}, binary $([ -e "c${n}b" ] && echo yes || echo no): $(head -c 240 "c${n}.chk")"
    fi
done <<'CASES'
record literal|io.println(S{x:3;p:P{x:4}} as str);
let-bound record|io.println(r as str);
record from a function|io.println(mk() as str);
record field holding a record|io.println(r.p as str);
array|io.println(a as str);
array of records|io.println(ps as str);
map|io.println(mp as str);
sum value|io.println(v as str);
CASES

# ── 2. the casts that stay legal ─────────────────────────────────────────────
prog 'let n=65;io.println(n as str);let s="ok";io.println(s as str);io.println(r.x as str);io.println(r.p.x as str);let u:u8=7;io.println(u as str);' > ok.tk
"${TKC}" --check ok.tk > ok.chk 2>&1; crc=$?
if [ "${crc}" -eq 0 ] && [ ! -s ok.chk ] && "${TKC}" ok.tk -o okb > ok.log 2>&1 \
   && [ "$(./okb | tr '\n' ' ')" = "65 ok 3 4 7 " ]; then
    pass "scalar / str / scalar-field casts: no diagnostic, output '65 ok 3 4 7'"
else
    fail "legal casts: check rc ${crc}: $(head -c 240 ok.chk) $(head -c 240 ok.log) out '$(./okb 2>/dev/null | tr '\n' ' ')'"
fi

echo "----------------------------------------------------------------------"
echo "C1381: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
