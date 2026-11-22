#!/usr/bin/env bash
# C2032_str_substr_typed_record.sh — str.substr's interface record states the
# signature str.slice has (story 127.493).
#
# HISTORY.  stdlib/str.tki carried str.substr as an untyped `"gen":"abi"`
# placeholder (i64;i64;i64 -> i64), although it lowers to tk_str_substr_w,
# which is tk_str_slice_w: str.slice's fallible glue.  127.475 judged it as
# str!$sliceerr in the checker by name (std_substr_call); the record, the
# module docs and stdlib-signatures.md did not say so.  Two consequences of
# the placeholder, both measured on the base (b1333d8c):
#   - `mt str.substr(..)` was lowered against the RETURN VALUE (`icmp ne`)
#     instead of the error slot, so the `$err` binding was 0 and `e.msg` on
#     an out-of-range slice died with RT005 (nil dereference), as did a
#     `!$e` propagation's payload;
#   - a str or f64 passed where an index is read checked clean.
# The record is now `str;u64;u64 -> str!sliceerr`, the by-name rule is gone
# (the record carries the union), and the module docs and stdlib-signatures
# list the function.
#
# WHAT IS ASSERTED
#   1. the str.tki record: params str,u64,u64, return str!sliceerr, no "gen"
#      -- the same as str.slice's
#   2. `mt str.substr(s;3;99){$ok:v v;$err:e e.msg}` prints the slice error's
#      message (RT005 before); an in-range one takes $ok; an empty slice is ok
#   3. `<str.substr(t;0;9)!$e` propagates in a fallible function
#   4. --check: str.substr used as a plain str is E4031 got 'str!$sliceerr'
#      (127.475 kept), and reports the same union str.slice reports
#   5. --check: a str and an f64 index are E4031 expected 'u64' (clean
#      before); i64 locals and literals as indices stay clean
#   6. no `fix` on any of those E4031s
#
# Negative control: TKC=<base tkc> TKC_STDLIB_DIR=<base>/src/stdlib fails
# 1, 2 and 3 (both RT005) and 5; 4 and 6 pass on both.
#
# Story: 127.493
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
TKI="${TKC_STDLIB_DIR}/../../stdlib/str.tki"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2032_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT

# 1. the record
rec() {
    python3 - "${TKI}" "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for e in d.get("exports", d if isinstance(d, list) else []):
    if isinstance(e, dict) and e.get("name") == sys.argv[2]:
        print(",".join(e.get("params", [])), e.get("return"), e.get("gen", "-"))
        break
PY
}
expect "1-record-substr" "$(rec str.substr)" "str,u64,u64 str!sliceerr -"
expect "1-record-same-as-slice" "$(rec str.substr)" "$(rec str.slice)"

# 2. run time
cat > "${WORK}/r.tk" <<'TK'
m=main;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let s="hello";
  io.println(mt str.substr(s;3;99){$ok:v v;$err:e e.msg});
  io.println(mt str.substr(s;1;3){$ok:v "ok:\(v)";$err:e e.msg});
  io.println(mt str.substr(s;2;2){$ok:v "ok:[\(v)]";$err:e e.msg});
  <0
};
TK
if "${TKC}" -o "${WORK}/r" "${WORK}/r.tk" >"${WORK}/r.log" 2>&1; then
    got="$("${WORK}/r" 2>&1 | tr '\n' '|')"
else
    got="build failed: $(head -c 300 "${WORK}/r.log")"
fi
expect "2-mt-err-binds-message" "${got}" "slice index out of range|ok:el|ok:[]|"

# 3. propagation
cat > "${WORK}/p.tk" <<'TK'
m=main;
i=io:std.io;
i=str:std.str;
t=$e{msg:$str};
f=k(t:$str):$str!$e{<str.substr(t;0;9)!$e};
f=main():i64{
  io.println(mt k("abc"){$ok:v v;$err:e "E:\(e.msg)"});
  io.println(mt k("abcdefghijk"){$ok:v v;$err:e "E:\(e.msg)"});
  <0
};
TK
if "${TKC}" -o "${WORK}/p" "${WORK}/p.tk" >"${WORK}/p.log" 2>&1; then
    got="$("${WORK}/p" 2>&1 | tr '\n' '|')"
else
    got="build failed: $(head -c 300 "${WORK}/p.log")"
fi
expect "3-bang-propagates" "${got}" "E:slice index out of range|abcdefghi|"

# 4-6. --check
ALL=""
chk() {   # <name> <body> -> "CODE expected got|..."
    printf 'm=q;i=io:std.io;i=str:std.str;\nf=main():i64{let s="abc";let a=1;%s<0};\n' "$2" > "${WORK}/$1.tk"
    local out; out="$("${TKC}" --check "${WORK}/$1.tk" 2>&1)"
    ALL="${ALL}${out}"$'\n'
    printf '%s\n' "${out}" | sed -n 's/.*"error_code":"\([A-Z0-9]*\)".*"expected":"\([^"]*\)","got":"\([^"]*\)".*/\1 \2 \3/p' | tr '\n' '|'
}
expect "4-substr-as-str"  "$(chk c4a 'io.println(str.substr(s;0;1));')" 'E4031 str str!$sliceerr|'
expect "4-same-as-slice"  "$(chk c4b 'io.println(str.substr(s;0;1));')" "$(chk c4c 'io.println(str.slice(s;0;1));')"
expect "5-str-index"      "$(chk c5a 'let n=str.substr(s;"x";2);')" 'E4031 u64 str|'
expect "5-f64-index"      "$(chk c5b 'let n=str.substr(s;1.5;2);')" 'E4031 u64 f64|'
expect "5-i64-indices-ok" "$(chk c5c 'let n=mt str.substr(s;a;a+1){$ok:v v;$err:e ""};io.println(n);')" ''
nfix="$(printf '%s' "${ALL}" | grep -c '"fix"')"
expect "6-no-fix" "${nfix}" "0"

echo "C2032: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
