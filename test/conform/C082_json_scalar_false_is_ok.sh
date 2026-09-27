#!/usr/bin/env bash
# C082_json_scalar_false_is_ok.sh — a JSON false, 0 or 0.0 is a value, not an
# error (story 127.193).
#
# THE DEFECT.  std.json's scalar accessors return the value itself and
# signalled failure with the 0/null sentinel, so a stored false was
# indistinguishable from a failure:
#
#     mt json.getbool(o;"off"){$ok:v v;$err:e true}     -> true   (wrong)
#
# test/standalone/test_json_full.tk printed "FAIL json.getbool false" and was
# quarantined in the codegen baseline for it.  Measured before the fix, the
# same wrong arm for json.bool of false and for json.i64 / json.getint of 0.
#
# THE FIX.  The wrappers keep @tk_current_error truthful on every return
# (json_glue.c), and llvm.c's is_stdlib_slot_wrapper() makes the match decide
# on the slot for exactly those wrappers — runtime-abi.md §7.2: "the zero
# filler is not the discriminant".
#
# BOTH DIRECTIONS.  A fix that always took $ok would pass the false cases, so
# the missing-key cases must still take $err; and the let-bound spelling is
# asserted as well as the direct one, because 135.16 carries the convention
# through the binding and the two must agree.
#
# Story: 127.193

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

WORK="$(mktemp -d /tmp/tkc_c082_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C082: json scalar accessors: false / 0 / 0.0 are ok, a missing key is err"
echo "--------------------------------------"

cat > p.tk <<'EOF'
m=p;
i=json:std.json;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let o=mt json.dec("{\"on\":true,\"off\":false,\"n\":0,\"x\":0.0,\"s\":\"t\"}"){$ok:v v;$err:e 0};
  io.println(str.concat("getbool off ";mt json.getbool(o;"off"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("getbool on ";mt json.getbool(o;"on"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("bool off ";mt json.bool(o;"off"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("i64 n ";mt json.i64(o;"n"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("getint n ";mt json.getint(o;"n"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("f64 x ";mt json.f64(o;"x"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("getbool missing ";mt json.getbool(o;"nope"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("i64 missing ";mt json.i64(o;"nope"){$ok:v "ok";$err:e "err"}));
  io.println(str.concat("i64 of a string ";mt json.i64(o;"s"){$ok:v "ok";$err:e "err"}));
  let b=json.bool(o;"off");
  let z=json.i64(o;"nope");
  io.println(str.concat("let bool off ";mt b{$ok:v "ok";$err:e "err"}));
  io.println(str.concat("let i64 missing ";mt z{$ok:v "ok";$err:e "err"}));
  <0
};
EOF

EXPECTED='getbool off ok
getbool on ok
bool off ok
i64 n ok
getint n ok
f64 x ok
getbool missing err
i64 missing err
i64 of a string err
let bool off ok
let i64 missing err'

if ! "${TKC}" --allow-all --out p_bin p.tk >compile.log 2>&1; then
    echo "  FAIL: program did not compile"
    grep -o '"message":"[^"]*"' compile.log | head -2 | sed 's/^/      /'
    FAIL=$((FAIL + 1))
else
    out="$(./p_bin 2>&1)"
    while IFS= read -r want; do
        if printf '%s\n' "${out}" | grep -qxF "${want}"; then
            echo "  PASS: ${want}"
            PASS=$((PASS + 1))
        else
            got="$(printf '%s\n' "${out}" | awk -v p="${want% *} " 'index($0,p)==1' | head -1)"
            echo "  FAIL: expected [${want}], got [${got}]"
            FAIL=$((FAIL + 1))
        fi
    done <<< "${EXPECTED}"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
