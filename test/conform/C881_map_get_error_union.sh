#!/usr/bin/env bash
# C881_map_get_error_union.sh — a map lookup `m.get(k)` is an error union
# `V!$err` to the checker, judged as a user `T!E` is (story 127.244, owner
# decision 127.30: "return error unions").  This is the map slice of
# 127.244; the std `T!E` calls (str.toint, json.*) are not enforced yet --
# measured too breaking to land before a migration exists (see the row).
#
# THE DEFECT (ed88ff66).  tk_map_get reports a miss on the error slot
# (127.30), but the checker typed the lookup as the plain value V:
#   * `m.get(k)+1`, `let v=m.get(k); v*2`, a lookup passed to an i64
#     parameter or returned as an i64 checked clean and evaluated the zero
#     filler on a miss;
#   * `!` on `m.get(k)` was E3020 "applied to a non-error-union value";
#   * `mt bm.get(k){$ok:v ..;$err:e ..}` on a bool-valued map was E4010
#     "missing arm for 'true'/'false'".
#   And E4031 printed only the inner type of any `T!E`: "expected 'i64', got
#   'i64'" for a user `g():i64!$e` passed as an i64.
#
# THE FIX.  types.c gives the lookup a TY_ERROR_TYPE `V!$err` carrying
# `slot_lower` = V, which infer() stores as the node's rtype, so an accepted
# program lowers exactly as it did.  A lookup used as the base of a field
# read or a subscript (`mm.get(k).get(0)`) is read as V and not judged yet:
# its repairs are unsound for compound values (new stories).  `!` on a lookup
# propagates on the error channel (llvm.c), never on the value, so a stored
# 0/false is a hit.  type_name renders a union as `T!$E`.  No fix on any of
# these: `mt`, `!` and `getor` are all repairs.
#
# Negative control: C881_TKC=<other tkc>.  With ed88ff66 + 127.313 (the
# branch base): see the commit message.
#
# Story: 127.244
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C881_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c881_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C881: a map lookup is judged as the error union V!\$err (127.244)"
echo "---------------------------------------------------------------------------"

# rejects name description got-substring source -- E4031 naming the union, no fix
rejects() {
    local name=$1 desc=$2 got=$3 out rc
    printf '%s\n' "$4" > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031"' \
       && printf '%s' "${out}" | grep -qF "${got}" \
       && ! printf '%s' "${out}" | grep -q '"fix"'; then
        echo "  PASS: ${desc} (E4031 '${got}', no fix)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected E4031 naming '${got}' without a fix, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
    fi
}
clean() {     # name description source
    local name=$1 desc=$2 out rc
    printf '%s\n' "$3" > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ -z "${out}" ]; then
        echo "  PASS: ${desc} (clean)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected clean, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
    fi
}
runs() {      # name description expected-output source
    local name=$1 desc=$2 want=$3 got
    printf '%s\n' "$4" > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed"; fi
    if [ "${got}" = "${want}" ]; then echo "  PASS: ${desc} (${want})"; PASS=$((PASS + 1))
    else echo "  FAIL: ${desc}: expected '${want}', got '${got}'"; FAIL=$((FAIL + 1)); fi
}

H='i=io:std.io;i=str:std.str;i=json:std.json;t=$e{msg:$str};'
M='let m=@("a":0;"b":5);'
O='let o=mt json.dec("{}"){$ok:v v;$jsonerr:e json.newobj()};'

# ── the union used as its value ──────────────────────────────────────────
rejects v2 'm.get("a")+1' "i64!\$err" \
  "m=v2;${H}f=main():i64{${M}let x=m.get(\"a\")+1;<0};"
rejects v5 'let v=m.get(k); v*2 (let-bound)' "i64!\$err" \
  "m=v5;${H}f=main():i64{${M}let v=m.get(\"a\");let x=v*2;<0};"
rejects v6 'a lookup passed to an i64 parameter' "got 'i64!\$err'" \
  "m=v6;${H}f=h(a:i64):i64{<a};f=main():i64{${M}<h(m.get(\"a\"))};"
rejects v7 'a lookup returned as an i64' "got 'i64!\$err'" \
  "m=v7;${H}f=k(m:@(\$str:i64)):i64{<m.get(\"a\")};f=main():i64{<0};"
rejects v8 'a std T!E into an i64 annotation (127.304)' "got 'i64!parseerr'" \
  "m=v8;${H}f=main():i64{let x:i64=str.toint(\"3\");<x};"
# ── E4031 names both sides of a user T!E (it printed 'i64', got 'i64') ──
rejects v12 'a mut binding of a lookup is the union too (as for a user T!E)' "i64!\$err" \
  "m=v12;${H}f=main():i64{${M}let x=mut.m.get(\"a\");let y=x+1;<0};"
rejects v11 'a user g():i64!$e passed as an i64' "expected 'i64', got 'i64!\$e'" \
  "m=v11;${H}f=g():i64!\$e{<1};f=h(a:i64):i64{<a};f=main():i64{<h(g())};"

# ── the union consumed ───────────────────────────────────────────────────
clean c1 'mt on a bool-valued lookup with $ok/$err arms is exhaustive' \
  "m=c1;${H}f=main():i64{let bm=@(\"t\":true);let x=mt bm.get(\"t\"){\$ok:v 1;\$err:e 2};<x};"
clean c2 '! on a lookup inside a T!E function' \
  "m=c2;${H}f=k(m:@(\$str:i64)):i64!\$e{let x=m.get(\"a\")!\$e;<x+1};f=main():i64{<0};"
clean c3 'getor, mt into an annotation, and ! on a std T!E' \
  "m=c3;${H}f=k(s:\$str):i64!\$e{<str.toint(s)!\$e};f=main():i64{${M}let x=m.getor(\"a\";0)+1;let n:i64=mt str.toint(\"3\"){\$ok:v v;\$err:e 0};<x+n};"

# ── and runs on the error channel ────────────────────────────────────────
runs r1 '! on a lookup: a stored 0 or false is a hit, a miss re-raises' '1 -1 1 2 ' \
  "m=r1;${H}
f=k(m:@(\$str:i64);key:\$str):i64!\$e{let x=m.get(key)!\$e;<x+1};
f=kb(m:@(\$str:bool);key:\$str):bool!\$e{<m.get(key)!\$e};
f=main():i64{
  let m=@(\"a\":0);let bm=@(\"f\":false);
  io.println(\"\\(mt k(m;\"a\"){\$ok:v v;\$e:e 0-1}) \\(mt k(m;\"z\"){\$ok:v v;\$e:e 0-1})\");
  io.println(\"\\(mt kb(bm;\"f\"){\$ok:v 1;\$e:e 2}) \\(mt kb(bm;\"q\"){\$ok:v 1;\$e:e 2})\");
  <0};"

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
