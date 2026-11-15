#!/usr/bin/env bash
# C1832_recv_slice_errunion.sh — the receiver spelling of a fallible std
# function is the error union its module form is (story 127.462, with
# 127.319 / 127.323, register 1.1 (a), owner 2026-11-15).
#
# THE DEFECT (main 685aa852, v3.2.0, and 127.319 as first built).
# `t.slice(a;b)`, `t.substr(a;b)` and `t.substring(a;b)` on a str value lower
# to tk_str_slice_w (tk_str_substr_w calls it) -- the glue `str.slice(t;a;b)`
# lowers to, which reports an out-of-range slice on the error slot (127.241):
# `mt t.slice(2;9){..}` takes the $err arm.  127.319 judged the module form
# as the union `str!$sliceerr`, but the checker typed the receiver form a
# plain str (127.364), so `io.println(t.slice(2;9))` checked clean and
# printed the null filler, and rewriting `str.slice(t;a;b)` to `t.slice(a;b)`
# silenced every E4031 the module form draws.  `t.slice(0;9)!$e` was E3020
# ("! applied to a non-error-union value") although the call is fallible.
#
# THE FIX.  types.c recv_slot_union: on an ESTABLISHED str receiver those
# three members are the union `str!$sliceerr`, with slot_lower = the str the
# node had, so codegen does not move.  Every position that judges the module
# form judges it: a std or user argument, an interpolation, a comparison, an
# `as` cast, a typed binding, a return, a `let` binding read later.  No
# `fix`.  An array `.slice` (tk_arr_slice_w, which clamps) is not fallible
# and stays a plain array.
#
# Negative control: C1832_TKC=<v3.2.0 tkc> -- every `rejects` case fails
# (each checks clean there) and so does `same-got`; clean c2 and runs u2 fail
# too (E3020 on `!`); c1, c3, c4 and u1 pass: 4 passed, 11 failed.
#
# Story: 127.462
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1832_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1832_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C1832: t.slice / t.substr / t.substring on a str are the union str.slice is (127.462)"
echo "---------------------------------------------------------------------------"

# rejects name description where-substring source -- E4031 naming str!$sliceerr, no fix
rejects() {
    local name=$1 desc=$2 where=$3 out rc
    printf '%s\n' "$4" > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031"' \
       && printf '%s' "${out}" | grep -qF "${where}" \
       && printf '%s' "${out}" | grep -qF '"got":"str!$sliceerr"' \
       && ! printf '%s' "${out}" | grep -q '"fix"'; then
        echo "  PASS: ${desc} (E4031 'str!\$sliceerr', no fix)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected E4031 [${where}] naming 'str!\$sliceerr' without a fix, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
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

H='i=io:std.io;i=str:std.str;t=$e{msg:$str};'

# ── the receiver spelling, read as a plain str ───────────────────────────
rejects r1 'io.println(t.slice(2;9)) -- a std argument' "argument 1 of 'io.println'" \
  "m=r1;${H}f=main():i64{let t=\"abc\";io.println(t.slice(2;9));<0};"
rejects r2 'let u=t.slice(0;1); u=="a" -- a comparison' "a comparison '=='" \
  "m=r2;${H}f=main():i64{let t=\"abc\";let u=t.slice(0;1);if(u==\"a\"){<1};<0};"
rejects r3 '"\(t.substr(0;1))" -- an interpolation' "a string interpolation" \
  "m=r3;${H}f=main():i64{let t=\"abc\";io.println(\"[\\(t.substr(0;1))]\");<0};"
rejects r4 'let u:$str=t.substring(0;1) -- a typed binding' "type mismatch: expected 'str'" \
  "m=r4;${H}f=main():i64{let t=\"abc\";let u:\$str=t.substring(0;1);<0};"
rejects r5 '<t.slice(0;1) from a $str function -- a return' "type mismatch: expected 'str'" \
  "m=r5;${H}f=k(t:\$str):\$str{<t.slice(0;1)};f=main():i64{<0};"
rejects r6 't.slice(0;1) as $str -- an as cast' "an 'as' cast" \
  "m=r6;${H}f=main():i64{let t=\"abc\";let u=t.slice(0;1) as \$str;<0};"
rejects r7 'a user $str parameter, from a literal receiver' "type mismatch: expected 'str'" \
  "m=r7;${H}f=h(a:\$str):i64{<0};f=main():i64{<h(\"abc\".slice(0;1))};"
rejects r8 'a receiver-method argument: xs.append(t.slice(0;1))' "argument 1 of the method '.append'" \
  "m=r8;${H}f=main():i64{let t=\"abc\";let xs=mut.@(\"x\");xs=xs.append(t.slice(0;1));<0};"

# ── the same union as the module form ───────────────────────────────────
same_got() {
    local a b
    printf '%s\n' "m=sg1;${H}f=main():i64{let t=\"abc\";io.println(str.slice(t;2;9));<0};" > sg1.tk
    printf '%s\n' "m=sg2;${H}f=main():i64{let t=\"abc\";io.println(t.slice(2;9));<0};" > sg2.tk
    a="$("${TKC}" --check sg1.tk 2>&1 | grep -o '"got":"[^"]*"' | head -1)"
    b="$("${TKC}" --check sg2.tk 2>&1 | grep -o '"got":"[^"]*"' | head -1)"
    if [ -n "${a}" ] && [ "${a}" = "${b}" ]; then
        echo "  PASS: str.slice(t;a;b) and t.slice(a;b) report the same union (${a})"; PASS=$((PASS + 1))
    else
        echo "  FAIL: module form ${a:-none} vs receiver form ${b:-none}"; FAIL=$((FAIL + 1))
    fi
}
same_got

# ── consumed, and the clamping array slice: clean ────────────────────────
clean c1 'mt t.slice(..){$ok:v v;$err:e ""}' \
  "m=c1;${H}f=main():i64{let t=\"abc\";let u=mt t.slice(0;1){\$ok:v v;\$err:e \"\"};io.println(u);<0};"
clean c2 't.slice(..)!$e in a fallible function (E3020 before)' \
  "m=c2;${H}f=k(t:\$str):\$str!\$e{<t.slice(0;9)!\$e};f=main():i64{<0};"
clean c3 'a let-bound receiver slice matched later' \
  "m=c3;${H}f=main():i64{let t=\"abc\";let r=t.substr(0;1);io.println(mt r{\$ok:v v;\$err:e \"E\"});<0};"
clean c4 'an array .slice clamps and is not fallible' \
  "m=c4;${H}f=main():i64{let a=@(1;2;3);let b=a.slice(1;9);io.println(\"\\(b.len)\");<0};"

# ── and lowers as it did ─────────────────────────────────────────────────
runs u1 'mt on t.slice / t.substr / t.substring: ok and err values' 'b E b E b E ' \
  "m=u1;${H}
f=main():i64{
  let t=\"abc\";
  io.println(mt t.slice(1;2){\$ok:v v;\$err:e \"E\"});
  io.println(mt t.slice(2;9){\$ok:v v;\$err:e \"E\"});
  io.println(mt t.substr(1;2){\$ok:v v;\$err:e \"E\"});
  io.println(mt t.substr(2;9){\$ok:v v;\$err:e \"E\"});
  io.println(mt t.substring(1;2){\$ok:v v;\$err:e \"E\"});
  io.println(mt t.substring(2;9){\$ok:v v;\$err:e \"E\"});
  <0};"
runs u2 't.slice(..)!$e propagates the failure' 'ok:a err ' \
  "m=u2;${H}
f=k(t:\$str;n:i64):\$str!\$e{<t.slice(0;n as u64)!\$e};
f=main():i64{
  io.println(mt k(\"abc\";1){\$ok:v \"ok:\\(v)\";\$err:e \"err\"});
  io.println(mt k(\"abc\";9){\$ok:v \"ok:\\(v)\";\$err:e \"err\"});
  <0};"

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
