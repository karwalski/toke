#!/usr/bin/env bash
# C1920_mt_variant_arms_on_scalar.sh — a `mt` whose arms name variants, over
# a value known to be a plain integer, is refused at --check (story 138.37,
# loke T-66).
#
# THE DEFECT.  `f=h(e:i64):$r{<mt e {$notfound:v ..;$permission:v ..;$io:v
# ..}}` passed `--check` (rc 0) and the build stopped with E9005 "match has 3
# arms, but the sum type of the matched value is not established"; at 2 arms
# the build stopped with the 2-arm E9005, and a u8 subject gave invalid IR.  A
# check-only target never saw it.  3.2.0 closed the same class for an unknown
# label (E3011, 127.292) and a record subject (E4080, 127.367).
#
# NOW.  E4031 at the first variant label, `expected` "a sum type", `got` the
# scalar type, and no `fix` (a sum-typed value or integer-literal arms may have
# been meant).  Judged only where the checker is certain the subject is a
# non-union scalar (a parameter or immutable `let` of a scalar type, a literal,
# a call returning a scalar) -- the predicate 127.202 uses for `$ok`/`$err`
# over a scalar.
#
# Negative controls kept: a sum match, a string-tag match over a str, a
# type-named `$i64:v` arm, a `mut` scrutinee (never judged), and a match on a
# T!E call still check clean.
#
# Negative control: v3.2.0 (ea7ddf22) — cases 1-4 check clean (rc 0).
#
# Story: 138.37

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
[ -x "${TKC}" ] || { echo "FAIL: tkc binary not found at ${TKC}"; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1920_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1920: variant arms on a scalar subject are refused at --check"

# refused NAME LINE:COL GOT SOURCE
refused() {
    local name="$1" at="$2" got="$3"
    printf '%s\n' "$4" > "${WORK}/${name}.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ]; then fail "${name}: --check passed it"; return; fi
    local rec; rec="$(printf '%s\n' "${out}" | grep -m1 '"error_code":"E4031"')"
    if [ -z "${rec}" ]; then fail "${name}: no E4031: $(printf '%s' "${out}" | head -c 200)"; return; fi
    local line="${at%%:*}" col="${at##*:}"
    printf '%s' "${rec}" | grep -q "\"line\":${line},\"col\":${col}" \
        && pass "${name}: E4031 at ${at}" || fail "${name}: wrong position: ${rec}"
    printf '%s' "${rec}" | grep -q '"expected":"a sum type"' \
        && printf '%s' "${rec}" | grep -q "\"got\":\"${got}\"" \
        && pass "${name}: expected 'a sum type', got '${got}'" \
        || fail "${name}: expected/got wrong: ${rec}"
    printf '%s' "${rec}" | grep -q '"fix"' && fail "${name}: carries a fix" \
        || pass "${name}: no fix"
}
clean() {
    printf '%s\n' "$2" > "${WORK}/$1.tk"
    local out
    if out="$("${TKC}" --check "${WORK}/$1.tk" 2>&1)"; then pass "$1: checks clean"
    else fail "$1: rejected: $(printf '%s' "${out}" | grep -m1 -o '"message":"[^"]*"')"; fi
}

# 1. loke's T-66 reproducer: three FileErr-like labels over an i64 parameter.
refused t66 5:6 i64 'm=t66;
t=$r{ ok:bool; msg:str };
f=h(e:i64):$r{
  <mt e {
    $notfound:v $r{ok:false;msg:"nf"};
    $permission:v $r{ok:false;msg:"pd"};
    $io:v $r{ok:false;msg:"io"}
  }
};
f=main():i64{ <0 };'
# 2. two arms (the 2-arm E9005 at build).
refused two 2:24 i64 'm=t;
f=h(e:i64):i64{<mt e {$a:v 1;$b:v 2}};
f=main():i64{<h(1)};'
# 3. an immutable let bound to a literal.
refused letlit 2:31 i64 'm=t;
f=main():i64{let e=5; <mt e {$a:v 1;$b:v 2;$c:v 3}};'
# 4. a u8 parameter, one arm (invalid IR at build).
refused u8one 2:23 u8 'm=t;
f=h(e:u8):i64{<mt e {$a:v 1}};
f=main():i64{<h(4)};'

# Controls: still clean.
clean sum 'm=t;
t=$sh{$circle:i64;$sq:i64};
f=a(s:$sh):i64{<mt s {$circle:r r;$sq:w w}};
f=main():i64{<a($sh{$sq:2})};'
clean strtag 'm=t;
f=h(e:str):i64{<mt e {$aa:w 1;$bb:w 2}};
f=main():i64{<h("aa")};'
clean typenamed 'm=t;
f=h(e:i64):i64{<mt e {$i64:v v}};
f=main():i64{<h(3)};'
clean mutscr 'm=t;
f=main():i64{let e=mut.5; e=6; <mt e {$a:v 1;$b:v 2;$c:v 3}};'
clean fallible 'm=t;
i=str:std.str;
f=main():i64{<mt str.toint("7") {$ok:v v;$err:e 0}};'

echo "C1920: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
