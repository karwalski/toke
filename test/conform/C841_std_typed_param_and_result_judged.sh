#!/usr/bin/env bash
# C841_std_typed_param_and_result_judged.sh — a std call is judged in both
# directions against its typed .tki record (story 127.304).
#
# THE DEFECTS (both passed --check on main 7758dcf3).
#   * A str argument for a parameter the typed record declares `i64`:
#     `str.fromint("x")` built and printed the string's address.
#     check_tki_arg_types skipped every `i64` parameter, because hand-written
#     records use `i64` for handles too.
#   * A record or `T!E` std result bound to a scalar-annotated `let`:
#     `let s:str=json.at(o;"a")` (`jsonval!jsonerr`) and
#     `let s:str=json.newobj()` (`jsonval`) built and printed a pointer.
#     stdlib results are not adopted as binding types (127.66) and 127.232
#     judged only a declared SCALAR return.
#   The reverse directions were already judged (i64 into a declared str,
#   str into a declared jsonval) -- C760 asserts those.
#
# THE FIX.  check_tki_arg_types judges a `str` argument against an `i64`
#   parameter (a str is never a handle; any other argument there is still
#   not judged).  The NODE_BIND_STMT check reports E4031 when a scalar
#   annotation meets a std call whose typed record declares a record or a
#   `T!E` union -- the rule a user `T!E` call into a `T` let already has.
#   No `fix` either way: an `mt`, a `!`, a conversion or another callee are
#   all possible repairs.
#
#   The typed records that spelled a str parameter `i64` are retyped `str`:
#   json.getint/geti64/geti32/getbool/getf32/getfloat (key), http.resok,
#   resbad, reserr (body / message) and http.resjson (body).  Measured on the
#   tree (test/, examples/, docs/, ooke main, toke-test-programs: 3,800+
#   files), the E4031 set is identical before and after this change.
#
# Negative control: C841_TKC=<other tkc> runs the cases against another
# compiler (this tree's .tki).  Measured 2026-11-09 with main 7758dcf3's
# compiler: 7 passed, 6 failed -- exactly the six compiler-side rejections
# (p5 is carried by the retyped .tki alone).
#
# Story: 127.304
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C841_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c841_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C841: std call parameters and results judged against the typed .tki (127.304)"
echo "---------------------------------------------------------------------------"

rejects() {   # name description source -- E4031, no fix
    local name=$1 desc=$2 out rc
    printf '%s\n' "$3" > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031"' \
       && ! printf '%s' "${out}" | grep -q '"fix"'; then
        echo "  PASS: ${desc} (E4031, no fix)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected E4031 without a fix, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
    fi
}
clean() {     # name description source
    local name=$1 desc=$2 out rc
    printf '%s\n' "$3" > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ]; then
        echo "  PASS: ${desc} (clean)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected clean, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
    fi
}

H='i=io:std.io;i=str:std.str;i=json:std.json;'
D='let o=mt json.dec("{\"a\":5}") { $ok:v v; $jsonerr:e json.newobj() };'

# ── direction 1: a str argument for a declared i64 parameter ─────────────
rejects p1 'str.fromint("x") -- str.tki declares i64' \
  "m=p1;${H}f=main():i64{let s=str.fromint(\"x\");<0};"
rejects p2 'a str-typed name for a declared i64' \
  "m=p2;${H}f=main():i64{let t=\"9\";let s=str.fromint(t);<0};"
clean   p3 'str.fromint(5) and str.fromint(n) with n:i64' \
  "m=p3;${H}f=main():i64{let n=7;let s=str.fromint(5);let u=str.fromint(n);<0};"

clean   p4 'the retyped str parameters: json.getint key, http.resjson body' \
  "m=p4;${H}i=http:std.http;f=main():i64{${D}let n=mt json.getint(o;\"a\") { \$ok:v v; \$jsonerr:e 0 };let r=http.resjson(200;\"{}\");<0};"
rejects p5 'an i64 key for json.getint (now declared str)' \
  "m=p5;${H}f=main():i64{${D}let n=mt json.getint(o;7) { \$ok:v v; \$jsonerr:e 0 };<0};"

# ── direction 2: a record / T!E std result into a scalar-annotated let ───
rejects r1 'let s:str=json.at(..) -- declares jsonval!jsonerr' \
  "m=r1;${H}f=main():i64{${D}let s:str=json.at(o;\"a\");<0};"
rejects r2 'let s:str=json.newobj() -- declares jsonval' \
  "m=r2;${H}f=main():i64{let s:str=json.newobj();<0};"
rejects r3 'let n:i64=json.newobj() -- a record into a number' \
  "m=r3;${H}f=main():i64{let n:i64=json.newobj();<0};"
rejects r4 'let b:bool=json.i64(..) -- declares i64!jsonerr' \
  "m=r4;${H}f=main():i64{${D}let b:bool=json.i64(o;\"a\");<0};"
clean   r5 'un-annotated lets of a record / T!E std result' \
  "m=r5;${H}f=main():i64{${D}let v=json.at(o;\"a\");let w=json.newobj();<0};"
clean   r6 'a record std result into its own record annotation' \
  "m=r6;${H}f=main():i64{let w:\$jsonval=json.newobj();<0};"
clean   r7 'the T!E unwrapped by mt into a scalar annotation' \
  "m=r7;${H}f=main():i64{${D}let n:i64=mt json.i64(o;\"a\") { \$ok:v v; \$jsonerr:e 0 };<0};"
clean   r8 'a declared scalar return into its scalar annotation' \
  "m=r8;${H}f=main():i64{let s:str=str.fromint(5);let n:u64=str.len(\"ab\");<0};"

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
