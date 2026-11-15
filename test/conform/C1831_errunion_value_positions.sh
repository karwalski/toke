#!/usr/bin/env bash
# C1831_errunion_value_positions.sh — an error union `T!E` read as its value
# in the five positions no rule judged (story 127.323, register 1.1 (a),
# owner 2026-11-15): an if/lp condition, the operand of `!`, a string
# interpolation, a std-call argument, and an assignment to a `mut` binding.
# Each holds for the three kinds of union: a user fallible call
# (`g():i64!$e`), a map lookup (`m.get(k)`, 127.244) and a std `T!E` call
# (127.319).
#
# THE DEFECT (main 685aa852, v3.2.0).  Every one of these checked clean and
# took the zero filler on a failure -- the defect 127.30 removed from `mt`:
#   if(gb()){..}  lp(gb()){..}  !gb()  "\(g())"  str.fromint(g())
#   let y=mut.0; y=g()
# (a typed target -- `let s=mut.""; s=gs()` -- was already judged).
#
# THE FIX.  types.c union_value_use: E4031 "expected 'T', got 'T!$E' --
# <position> reads the value of a fallible result", no `fix`.  The std-call
# argument covers a module call and a value-receiver method (std glue); a
# user-module call is left to its own argument rule.  The `mut` assignment is
# judged only when the binding is never consumed as a union anywhere in the
# function (an `mt` scrutinee, a `!` operand, a return, the whole value of
# another binding): 135.16a (C067) made `let r=mut.0; r=s.toint(x); mt r{..}`
# match on the assigned call's channel, and that program stays clean.
#
# Negative control: C1831_TKC=<v3.2.0 tkc> -- every `rejects` case except the
# user-union std argument into a `str` parameter (judged since 127.304) fails
# clean; the `clean` cases pass.
#
# Story: 127.323
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1831_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1831_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C1831: a T!E read as its value in a condition, '!', interpolation, std argument, mut assignment (127.323)"
echo "---------------------------------------------------------------------------"

# rejects name description where-substring got-substring source
rejects() {
    local name=$1 desc=$2 where=$3 got=$4 out rc
    printf '%s\n' "$5" > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031"' \
       && printf '%s' "${out}" | grep -qF "${where}" \
       && printf '%s' "${out}" | grep -qF "${got}" \
       && ! printf '%s' "${out}" | grep -q '"fix"'; then
        echo "  PASS: ${desc} (E4031 '${got}', no fix)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected E4031 [${where}] naming '${got}' without a fix, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
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

H='i=io:std.io;i=str:std.str;i=file:std.file;t=$e{msg:$str};f=g():i64!$e{<1};f=gb():bool!$e{<true};'
M='let bm=@("t":true);let m=@("a":1);'

# ── an if / lp condition ─────────────────────────────────────────────────
rejects i1 'if on a user bool union' "an 'if' condition" "bool!\$e" \
  "m=i1;${H}f=main():i64{if(gb()){<1};<0};"
rejects i2 'if on a bool map lookup' "an 'if' condition" "bool!\$err" \
  "m=i2;${H}f=main():i64{${M}if(bm.get(\"t\")){<1};<0};"
rejects i3 'if on a std bool union (file.write)' "an 'if' condition" "bool!\$fileerr" \
  "m=i3;${H}f=main():i64{if(file.write(\"/tmp/c1831_never/x\";\"\")){<1};<0};"
rejects l1 'lp(cond) on a user bool union' "a 'lp' condition" "bool!\$e" \
  "m=l1;${H}f=main():i64{lp(gb()){<1};<0};"
rejects l2 'lp(init;cond;step) on a bool map lookup' "a 'lp' condition" "bool!\$err" \
  "m=l2;${H}f=main():i64{${M}lp(let i=0;bm.get(\"t\");i=i+1){<1};<0};"
# ── the operand of ! ─────────────────────────────────────────────────────
rejects n1 '!gb()' "the operand of '!'" "bool!\$e" \
  "m=n1;${H}f=main():i64{let b=!gb();<0};"
rejects n2 '!bm.get(k)' "the operand of '!'" "bool!\$err" \
  "m=n2;${H}f=main():i64{${M}let b=!bm.get(\"t\");<0};"
rejects n3 '!file.delete(p)' "the operand of '!'" "bool!\$fileerr" \
  "m=n3;${H}f=main():i64{if(!file.delete(\"/tmp/c1831_never\")){<1};<0};"
# ── a string interpolation ───────────────────────────────────────────────
rejects s1 '"\(g())"' "a string interpolation" "i64!\$e" \
  "m=s1;${H}f=main():i64{io.println(\"\\(g())\");<0};"
rejects s2 '"\(m.get(k))"' "a string interpolation" "i64!\$err" \
  "m=s2;${H}f=main():i64{${M}io.println(\"v=\\(m.get(\"a\"))\");<0};"
rejects s3 '"\(n)" with n=str.toint(..)' "a string interpolation" "i64!\$parseerr" \
  "m=s3;${H}f=main():i64{let n=str.toint(\"4\");io.println(\"n=\\(n)\");<0};"
# ── a std-call argument ──────────────────────────────────────────────────
rejects a1 'str.fromint(g())' "argument 1 of 'str.fromint'" "i64!\$e" \
  "m=a1;${H}f=main():i64{io.println(str.fromint(g()));<0};"
rejects a2 'str.fromint(m.get(k))' "argument 1 of 'str.fromint'" "i64!\$err" \
  "m=a2;${H}f=main():i64{${M}io.println(str.fromint(m.get(\"a\")));<0};"
rejects a3 'io.println(str.slice(..))' "argument 1 of 'io.println'" "str!\$sliceerr" \
  "m=a3;${H}f=main():i64{io.println(str.slice(\"abc\";0;1));<0};"
rejects a4 'a receiver method argument: xs.append(str.toint(..))' "argument 1 of the method '.append'" "i64!\$parseerr" \
  "m=a4;${H}f=main():i64{let xs=mut.@(1);xs=xs.append(str.toint(\"2\"));<0};"
# ── an assignment to a mut binding ───────────────────────────────────────
rejects m1 'let y=mut.0; y=g()' "the assignment to 'y'" "i64!\$e" \
  "m=m1;${H}f=main():i64{let y=mut.0;y=g();<y};"
rejects m2 'let y=mut.0; y=m.get(k)' "the assignment to 'y'" "i64!\$err" \
  "m=m2;${H}f=main():i64{${M}let y=mut.0;y=m.get(\"a\");<y};"
rejects m3 'let y=mut.0; y=str.toint(..)' "the assignment to 'y'" "i64!\$parseerr" \
  "m=m3;${H}f=main():i64{let y=mut.0;y=str.toint(\"5\");<y+1};"

# ── consumed as a union: clean ───────────────────────────────────────────
clean c1 'C067: let r=mut.0; r=s.toint(..); mt r{..}' \
  "m=c1;${H}f=main():i64{let r=mut.0;r=str.toint(\"0\");io.println(mt r{\$ok:v \"ok|\\(v)\";\$err:e \"err\"});<0};"
clean c2 'a mut binding assigned a user union and propagated' \
  "m=c2;${H}f=k():i64!\$e{let y=mut.0;y=g();<y!\$e};f=main():i64{<0};"
clean c3 'conditions, ! and interpolation over mt' \
  "m=c3;${H}f=main():i64{${M}if(mt gb(){\$ok:v v;\$err:e false}){io.println(\"\\(mt m.get(\"a\"){\$ok:v v;\$err:e 0})\")};let b=!(mt bm.get(\"t\"){\$ok:v v;\$err:e false});<0};"
clean c4 'getor on a map lookup in a condition' \
  "m=c4;${H}f=main():i64{${M}if(bm.getor(\"t\";false)){<1};<0};"
clean c5 'a std argument unwrapped with !' \
  "m=c5;${H}f=k(s:\$str):\$str!\$e{<str.fromint(str.toint(s)!\$e)};f=main():i64{<0};"

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
