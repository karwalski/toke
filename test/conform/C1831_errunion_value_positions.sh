#!/usr/bin/env bash
# C1831_errunion_value_positions.sh — an error union `T!E` read as its value
# in the positions no rule judged (story 127.323, register 1.1 (a), owner
# 2026-11-15): an if/lp condition, the operand of `!`, a string
# interpolation, a std-call argument, an assignment to a `mut` binding, the
# operand of an `as` cast, and each operand of a comparison
# (== != < <= > >=).  Each holds for the three kinds of union: a user
# fallible call (`g():i64!$e`), a map lookup (`m.get(k)`, 127.244) and a std
# `T!E` call (127.319).
#
# THE DEFECT (main 685aa852, v3.2.0).  Every one of these checked clean and
# took the zero filler on a failure -- the defect 127.30 removed from `mt`:
#   if(gb()){..}  lp(gb()){..}  !gb()  "\(g())"  str.fromint(g())
#   let y=mut.0; y=g()    g() as i64    g()==g()    g()<g()
# (a typed target -- `let s=mut.""; s=gs()` -- was already judged, and so
# was a MIXED comparison, a union against a plain T; two unions of the same
# type passed types_compat and compared their fillers:
# `str.toint("x")==str.toint("y")` was true).
#
# THE FIX.  types.c union_value_use: E4031 "expected 'T', got 'T!$E' --
# <position> reads the value of a fallible result", no `fix`.  The std-call
# argument covers a module call and a value-receiver method (std glue); a
# user-module call is left to its own argument rule.  A comparison judges
# each union operand (two diagnostics for union==union), before any other
# rule.  The `mut` assignment is judged unless the binding is CONSUMED as a
# union somewhere in the function -- an `mt` scrutinee, the operand of a
# postfix `!`, or a `<` from a function whose return type is a union --
# because 135.16a (C067) made `let r=mut.0; r=s.toint(x); mt r{..}` match on
# the assigned call's channel.  `let z=r`, a bare `r;` or an arm value do not
# consume it (127.323 wave 2: they did, and `let z=r; <z+1` read the filler).
# The other reads of a consumed binding are NOT judged: a flow-insensitive
# read rule rejects C067's own guard `if(q>100){q=s.toint(..)}`, which reads
# the plain initial value before the assignment (an owner question, 127.323).
#
# Negative control: C1831_TKC=<v3.2.0 tkc> -- every `rejects` case fails
# (each checks clean there, or reports a mixed comparison without naming the
# position); the `clean` cases pass.
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
echo "C1831: a T!E read as its value in a condition, '!', interpolation, std argument, mut assignment, as, comparison (127.323)"
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

# rejects_n name description where got N source -- as rejects, and exactly N
# E4031 diagnostics (each union operand of a comparison is its own).
rejects_n() {
    local name=$1 desc=$2 where=$3 got=$4 want=$5 out rc n
    printf '%s\n' "$6" > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    n="$(printf '%s\n' "${out}" | grep -c '"error_code":"E4031"')"
    if [ "${rc}" -ne 0 ] && [ "${n}" -eq "${want}" ] \
       && printf '%s' "${out}" | grep -qF "${where}" \
       && printf '%s' "${out}" | grep -qF "${got}" \
       && ! printf '%s' "${out}" | grep -q '"fix"'; then
        echo "  PASS: ${desc} (${n} x E4031 '${got}', no fix)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected ${want} x E4031 [${where}] naming '${got}' without a fix, got ${n}, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
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
rejects m4 'let z=r does not consume the union: the assignment is judged' "the assignment to 'r'" "i64!\$parseerr" \
  "m=m4;${H}f=main():i64{let r=mut.0;r=str.toint(\"x\");let z=r;<z+1};"
rejects m5 'nor does a bare r; statement' "the assignment to 'r'" "i64!\$e" \
  "m=m5;${H}f=main():i64{let r=mut.0;r=g();r;<r+1};"
rejects m6 'nor does an arm value' "the assignment to 'r'" "i64!\$err" \
  "m=m6;${H}f=main():i64{${M}let r=mut.0;r=m.get(\"a\");let z=mt g(){\$ok:v r;\$err:e 0};<z};"
# ── the operand of an as cast ────────────────────────────────────────────
rejects x1 'g() as i64' "an 'as' cast" "i64!\$e" \
  "m=x1;${H}f=main():i64{let x=g() as i64;<x};"
rejects x2 'm.get(k) as i64' "an 'as' cast" "i64!\$err" \
  "m=x2;${H}f=main():i64{${M}let x=m.get(\"b\") as i64;<x};"
rejects x3 'str.toint(..) as i64 (the cast does not unwrap: it was 0)' "an 'as' cast" "i64!\$parseerr" \
  "m=x3;${H}f=main():i64{let x=str.toint(\"x\") as i64;io.println(str.fromint(x));<0};"
rejects x4 'an index cast a.get(str.toint(..) as i64)' "an 'as' cast" "i64!\$parseerr" \
  "m=x4;${H}f=main():i64{let a=@(1;2);let v=a.get(str.toint(\"1\") as i64);<v};"
# ── each operand of a comparison ─────────────────────────────────────────
rejects_n k1 'str.toint(..)==str.toint(..)' "a comparison '=='" "i64!\$parseerr" 2 \
  "m=k1;${H}f=main():i64{if(str.toint(\"x\")==str.toint(\"y\")){<1};<0};"
rejects_n k2 'str.slice(..)<str.slice(..)' "a comparison '<'" "str!\$sliceerr" 2 \
  "m=k2;${H}f=main():i64{let s=\"ab\";if(str.slice(s;0;1)<str.slice(s;1;2)){<1};<0};"
rejects_n k3 'm.get(a)==m.get(b)' "a comparison '=='" "i64!\$err" 2 \
  "m=k3;${H}f=main():i64{${M}if(m.get(\"x\")==m.get(\"y\")){<1};<0};"
rejects_n k4 'm.get(a)<m.get(b)' "a comparison '<'" "i64!\$err" 2 \
  "m=k4;${H}f=main():i64{${M}if(m.get(\"x\")<m.get(\"y\")){<1};<0};"
rejects_n k5 'g()==g()' "a comparison '=='" "i64!\$e" 2 \
  "m=k5;${H}f=main():i64{if(g()==g()){<1};<0};"
rejects_n k6 'g()>=g()' "a comparison '>='" "i64!\$e" 2 \
  "m=k6;${H}f=main():i64{if(g()>=g()){<1};<0};"
rejects_n k7 'a mixed comparison str.toint(..)!=5 names the position' "a comparison '!='" "i64!\$parseerr" 1 \
  "m=k7;${H}f=main():i64{if(str.toint(\"x\")!=5){<1};<0};"

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
clean c6 'a mut binding consumed by a < from a fallible function' \
  "m=c6;${H}f=k():i64!\$e{let y=mut.0;y=g();<y};f=main():i64{<0};"
clean c7 "C067's guard: a plain read BEFORE the assignment of a consumed binding" \
  "m=c7;${H}f=main():i64{let q=mut.7;if(q>100){q=str.toint(\"zz\")};io.println(mt q{\$ok:v \"ok|\\(v)\";\$err:e \"err\"});<0};"
clean c8 'comparisons and casts over mt' \
  "m=c8;${H}f=main():i64{let a=mt str.toint(\"1\"){\$ok:v v;\$err:e 0};if(a==(mt g(){\$ok:v v;\$err:e 0})){<a as i64};<0};"

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
