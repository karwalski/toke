#!/usr/bin/env bash
# C1620_json_opaque_handle_at_check.sh -- `j.print(x)` / `j.enc(x)` where x
# is, or holds, an opaque std handle (a .tki record with no fields: strbuf,
# vechandle, svgelem, securebuf) is a type error at --check (story 127.402).
#
# THE DEFECT: the handle's memory is the runtime's own C struct, not toke's
# i64-slot layout, so it has no JSON representation and codegen's descriptor
# path (llvm.c json_type_desc, 127.353/127.361) refuses it.  But it refused it
# only at build time, as E9002 "internal compiler error", after --check had
# said rc 0 -- a user type error reported as a compiler fault, which breaks
# the repair loop.
#
# NOW: the checker reports the argument as E4031 (type mismatch), as it does
# for an opaque handle passed where another kind is declared (138.21) and for
# `j.encode(handle)`, with no `fix`.  The build stops at the same E4031 and
# never reaches E9002, which stays the codegen backstop.  The handle is found
# through arrays and plain-record fields at any depth.  A MAP is not looked
# into and its behaviour is unchanged (127.363 is an owner decision): a map,
# a record holding a map and a map of handles still pass --check.
#
# Negative control: TKC built from fix/z181-std-handle-in-place (0d347948) --
# cases 1-12 and 14-16 red; 13, 17-19 green.
#
# Story: 127.402, 127.559

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

H='m=c1620;
i=io:std.io;
i=s:std.str;
i=vec:std.vec;
i=svg:std.svg;
i=sm:std.securemem;
i=j:std.json;
t=$hv{v:$vechandle;n:i64};
t=$outer{h:$hv;k:str};
t=$hm{m:@(str:i64)};
t=$pt{x:i64;y:str};'

# src NAME BODY [DECLS] -- write a program whose main runs BODY
src() {
    printf '%s\n%s\nf=main():i64{\nlet st=svg.style("red";"blue";1.0);\n%s\n<0};\n' "${H}" "${3:-}" "$2" > "${WORK}/$1.tk"
}
codes() {   # codes FILE [--check] -> "E... rc=N"
    local out rc
    if [ "${2:-}" = "--check" ]; then
        out="$(cd "${WORK}" && "${TKC}" --check "$1" 2>&1)"; rc=$?
    else
        rm -f "${WORK}/$1.bin"
        out="$(cd "${WORK}" && tk_build "${TKC}" "$1" -o "$1.bin" 2>&1)"; rc=$?
        [ "${rc}" -eq 142 ] && { tk_build_failed 142; return; }
    fi
    echo "$(printf '%s\n' "${out}" | grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' ')rc=${rc}"
}
chk() { src "$1" "$2" "${3:-}"; codes "$1.tk" --check; }
B="E4031 rc=1"

expect "1 j.print of a bound vechandle (reported)" "$(chk n1 'let v=vec.new(); j.print(v);')" "${B}"
expect "2 j.print of vec.new() in place"           "$(chk n2 'j.print(vec.new());')" "${B}"
expect "3 j.enc of a bound vechandle"              "$(chk n3 'let v=vec.new(); io.println(j.enc(v));')" "${B}"
expect "4 j.enc of vec.new() in place"             "$(chk n4 'io.println(j.enc(vec.push(vec.new();1)));')" "${B}"
expect "5 j.print of a strbuf"                     "$(chk n5 'let b=s.buf(); j.print(b);')" "${B}"
expect "6 j.enc of an svgelem"                     "$(chk n6 'let e=j.enc(svg.rect(0.0;0.0;1.0;1.0;st));')" "${B}"
expect "7 j.print of a securebuf"                  "$(chk n7 'j.print(sm.alloc(16;0));')" "${B}"
expect "8 j.print of an array of handles"          "$(chk n8 'let a=@(vec.new();vec.new()); j.print(a);')" "${B}"
expect "9 j.enc of a nested array of handles"      "$(chk n9 'let a=@(@(s.buf())); let e=j.enc(a);')" "${B}"
expect "10 j.enc of a record holding a handle"     "$(chk n10 'let r=$hv{v:vec.new();n:1}; let e=j.enc(r);')" "${B}"
expect "11 j.print of a record two levels deep"    "$(chk n11 'j.print($outer{h:$hv{v:vec.new();n:1};k:"a"});')" "${B}"
expect "12 j.print of a \$vechandle parameter"     "$(chk n12 'let z=shv(vec.new());' 'f=shv(v:$vechandle):i64{j.print(v);<0};')" "${B}"

# 13: values read from a handle, and plain records / arrays, are untouched
src p13 'let v=vec.push(vec.push(vec.new();4);5); j.print(vec.len(v)); j.print($pt{x:vec.get(v;1);y:"q"}); io.println(j.enc(@(1;2)));'
expect "13a control checks clean" "$(codes p13.tk --check)" "rc=0"
( cd "${WORK}" && tk_build "${TKC}" p13.tk -o p13.bin >/dev/null 2>&1 ) || tk_build_failed $?
r13="$([ -x "${WORK}/p13.bin" ] && tk_run "${WORK}/p13.bin" 2>&1 | tr '\n' ' ')"
expect "13b control runs" "${r13}" '2 {"x":5,"y":"q"} [1,2] '

# 14: the BUILD stops at the checker's E4031 and never reports E9002
src b14 'let v=vec.new(); j.print(v); io.println(j.enc(s.buf()));'
expect "14 build reports E4031, not E9002" "$(codes b14.tk)" "E4031 rc=1"

# 15: schema-conforming JSON, no fix field, the fields name the handle
src j15 'let v=vec.new(); j.print(v); let r=$hv{v:v;n:1}; let e=j.enc(r);'
j15="$(cd "${WORK}" && "${TKC}" --check j15.tk 2>&1 | python3 -c '
import json, sys
out = []
for l in sys.stdin.read().splitlines():
    if not l.startswith("{"): continue
    d = json.loads(l)
    if d.get("error_code") != "E4031": continue
    out.append("%s|%s|%s|%s|%s" % (d["stage"], "fix" in d, d.get("got"),
               d.get("expected"), "holds a" in d["message"]))
print(" ; ".join(out))')"
expect "15 JSON diagnostics: stage, no fix, got, expected, wording" "${j15}" \
    "type_check|False|vechandle|a value with a JSON representation|False ; type_check|False|hv|a value with a JSON representation|True"

# 16: the message names the member and the handle type
src m16 'let b=s.buf(); let e=j.enc(@(b));'
m16="$(cd "${WORK}" && "${TKC}" --check m16.tk 2>&1 | grep -c "argument 1 of 'j.enc' holds a 'strbuf', an opaque std handle")"
expect "16 message names member and handle" "${m16}" "1"

# 17-19: maps are out of scope (127.363, owner): --check unchanged
expect "17 j.print of a map (unchanged)"           "$(chk u17 'j.print(@("a":1));')" "rc=0"
expect "18 j.print of a record holding a map"      "$(chk u18 'j.print($hm{m:@("a":1)});')" "rc=0"
expect "19 j.print of a map of handles"            "$(chk u19 'j.print(@("k":vec.new()));')" "rc=0"

echo "C1620: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
