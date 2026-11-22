#!/usr/bin/env bash
# C1982_slice_field_elem_substr_union.sh — a str slice on a record FIELD or an
# array ELEMENT receiver, and the module form str.substr, are the union
# str!$sliceerr (story 127.475; extends 127.462, C1832).
#
# HISTORY.  127.462 judged `t.slice(a;b)` / `.substr` / `.substring` only when
# recv_value_type established a str receiver (an identifier, a literal, a
# call).  `p.name.slice(2;9)` on a `name:$str` field and `xs.get(0).slice(2;9)`
# on an `@($str)` element lower to the same tk_str_slice_w and checked clean,
# printing the null filler on a failure.  `str.substr(s;a;b)` lowers to
# tk_str_substr_w (= tk_str_slice_w) but its std.tki record is an untyped
# "gen":"abi" placeholder, so it was not judged either.  Codegen does not move
# (the receiver's inferred rtype only relabels an IR comment).
#
# WHAT IS ASSERTED (--check; each judged case is E4031 got 'str!$sliceerr',
# no fix)
#   1. field receiver, interpolated              -> E4031
#   2. element receiver, compared with ==        -> E4031 (one: the union side)
#   3. nested field receiver as a std argument   -> E4031
#   4. let-bound field .substring, read as str   -> E4031
#   5. module str.substr as a std argument       -> E4031
#   6. module str.substr under another alias     -> E4031
#   7. `mt` on each new shape                    -> clean, and at run time the
#      out-of-range slice takes $err, the in-range one $ok
#   8. controls, not judged: an @i64 field .slice (clamps), `.len` of an
#      element substr (base of a field read), a statement call, a user
#      function named substr, a map lookup in mt   -> clean
#   9. no `fix` on any of the E4031s above
#
# Negative control: TKC=<v3.2.0 tkc> fails 1-6 and 9 (checks clean); 7 and 8
# pass on both by design.
#
# Story: 127.475
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1982_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
H='m=q;i=io:std.io;i=str:std.str;t=$pt{name:$str;items:@$i64;sub:$in};t=$in{tag:$str};'
P='let p=$pt{name:"abc";items:@(1;2);sub:$in{tag:"x"}};let xs=@("abc";"de");let s="abc";'
ALLOUT=""
# judged <name> <body>: exactly one E4031 got str!$sliceerr
judged() {
    printf '%s\nf=main():i64{%s%s<0};\n' "${H}" "${P}" "$2" > "${WORK}/$1.tk"
    local out; out="$("${TKC}" --check "${WORK}/$1.tk" 2>&1)"
    ALLOUT="${ALLOUT}${out}"$'\n'
    local got; got="$(printf '%s\n' "${out}" | sed -n 's/.*"error_code":"\([A-Z0-9]*\)".*"got":"\([^"]*\)".*/\1 \2/p' | tr '\n' '|')"
    expect "$1" "${got}" 'E4031 str!$sliceerr|'
}
clean() {
    printf '%s\nf=main():i64{%s%s<0};\n' "${H}" "${P}" "$2" > "${WORK}/$1.tk"
    local out rc; out="$("${TKC}" --check "${WORK}/$1.tk" 2>&1)"; rc=$?
    expect "$1" "rc=${rc} ${out}" "rc=0 "
}
judged "1-field-interp"     'io.println("[\(p.name.slice(2;9))]");'
judged "2-elem-compare"     'if(xs.get(0).slice(0;1)=="a"){io.println("y")};'
judged "3-nested-field-arg" 'io.println(p.sub.tag.slice(0;1));'
judged "4-let-substring"    'let a=p.name.substring(0;1);io.println(a);'
judged "5-module-substr"    'io.println(str.substr(s;0;1));'
printf 'm=q;i=io:std.io;i=s2:std.str;\nf=main():i64{io.println(s2.substr("abc";0;1));<0};\n' > "${WORK}/6.tk"
o6="$("${TKC}" --check "${WORK}/6.tk" 2>&1)"; ALLOUT="${ALLOUT}${o6}"$'\n'
expect "6-module-substr-alias" "$(printf '%s\n' "${o6}" | sed -n 's/.*"error_code":"\([A-Z0-9]*\)".*"got":"\([^"]*\)".*/\1 \2/p')" 'E4031 str!$sliceerr'
cat > "${WORK}/7.tk" <<EOF
${H}
f=main():i64{${P}
  io.println(mt p.name.slice(2;9){\$ok:v "ok:\(v)";\$err:e "ERR"});
  io.println(mt xs.get(0).substr(0;2){\$ok:v "ok:\(v)";\$err:e "ERR"});
  io.println(mt str.substr(s;1;3){\$ok:v "ok:\(v)";\$err:e "ERR"});
  io.println(mt str.substr(s;0;9){\$ok:v "ok:\(v)";\$err:e "ERR"});
  <0};
EOF
if "${TKC}" --check "${WORK}/7.tk" >/dev/null 2>&1 && "${TKC}" "${WORK}/7.tk" --out "${WORK}/b7" >/dev/null 2>&1; then
    expect "7-mt-runs" "$("${WORK}/b7" | tr '\n' ' ')" "ERR ok:ab ok:bc ERR "
else expect "7-mt-runs" "build failed" "ERR ok:ab ok:bc ERR "; fi
clean "8a-array-field-slice" 'let r=p.items.slice(0;1);io.println("\(r.len)");'
clean "8b-elem-substr-len"   'let n=xs.get(1).substr(0;1).len;'
clean "8c-statement"         'str.substr(s;0;1);'
clean "8d-map-mt"            'let m=@("k":"abc");io.println(mt m.get("k"){$ok:v v;$err:e ""});'
printf 'm=q;i=io:std.io;f=substr(a:$str;b:i64;c:i64):$str{<a};\nf=main():i64{io.println(substr("abc";0;1));<0};\n' > "${WORK}/8e.tk"
o8e="$("${TKC}" --check "${WORK}/8e.tk" 2>&1)"; expect "8e-user-substr" "rc=$? ${o8e}" "rc=0 "
nfix="$(printf '%s' "${ALLOUT}" | grep -c '"error_code":"E4031"')"
wfix="$(printf '%s' "${ALLOUT}" | grep '"error_code":"E4031"' | grep -c '"fix"')"
expect "9-six-e4031-no-fix" "${nfix} ${wfix}" "6 0"
echo "C1982: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
