#!/usr/bin/env bash
# C421_json_str_glues_fill_the_slot.sh — json.dec / json.str / json.arr and
# str.slice / str.matchbracket / str.frombytes fill the error slot, and the
# str parse wrappers raise a ParseErr box, so every `$err:e` arm binds the
# payload its .tki declares (story 127.241, json and str parts).
#
# THE DEFECT.  These six glues were on stdlib_tables.c's bare-glue list: on
# failure they returned 0 and never touched @tk_current_error, so the match
# fell back to the value sentinel and the $err arm bound nothing.  Measured on
# main 8efba9e:
#   - `mt json.dec(s){…$err:e mt e{$parse:m …}}` did not compile (E9005,
#     "the sum type of the matched value is not established"), and the same
#     for json.str and json.arr: the Parse / Type / Missing distinction and
#     the parser's own message were unreachable from toke;
#   - `mt str.slice(s;5;9){…$err:e e.msg}` was RT005 (nil dereference), and so
#     was `e.msg` on a failed str.toint / str.tofloat, whose glue stored the
#     payload-less 1 although str.tki declares ParseErr{msg};
#   - str.frombytes of bytes that are not UTF-8 was $ok, although it is
#     declared str!EncodingErr and str.md documents exactly that failure.
#
# BOTH DIRECTIONS.  Every failing case is paired with a succeeding one whose
# value is the zero-ish filler ("" / an empty array / 0), which the sentinel
# would have misread, and a slot poisoned by an earlier failure precedes the
# ok cases, so a glue that forgot to CLEAR the slot fails too.  Each case is
# its own program (an RT005 would otherwise hide every later case), at -O0
# and -O2.
#
# Story: 127.241

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

WORK="$(mktemp -d /tmp/tkc_c421_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C421: json/str glues fill the slot; \$err binds the declared payload"
echo "--------------------------------------"

# run_case NAME EXPECTED BODY — BODY is main's statements; a poisoned slot
# (a failed str.toint) is bound first.
run_case() {
    local name="$1" expected="$2" body="$3"
    cat > c.tk <<EOF
m=p;
i=io:std.io;
i=str:std.str;
i=json:std.json;
f=main():i64{
  let poison=str.toint("zz");
  let o=mt json.dec("{\"s\":\"\",\"t\":\"x\",\"n\":0,\"a\":[]}"){\$ok:v v;\$err:e json.dec("{}")};
${body}
  <0
};
EOF
    local opt
    for opt in -O0 -O2; do
        rm -f c_bin
        if ! "${TKC}" --allow-all "${opt}" --out c_bin c.tk >compile.log 2>&1; then
            echo "  FAIL: ${name} ${opt}: did not compile: $(grep -o '"error_code":"[^"]*"' compile.log | head -1)"
            FAIL=$((FAIL + 1)); continue
        fi
        local out
        out="$(./c_bin 2>&1)"
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${opt}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${opt}: expected [${expected}], got [${out}]"
            FAIL=$((FAIL + 1))
        fi
    done
}

run_case "json.dec malformed binds Parse + msg" "parse malformed JSON" \
'  mt json.dec("{bad"){$ok:v io.println("ok");$err:e mt e{$parse:m io.println(str.concat("parse ";m));$type:m io.println("type");$missing:m io.println("missing")}};'

run_case "json.dec ok after a failure" "ok" \
'  mt json.dec("{}"){$ok:v io.println("ok");$err:e io.println("err")};'

run_case "json.str missing binds Missing + key" "missing zz" \
'  mt json.str(o;"zz"){$ok:v io.println("ok");$err:e mt e{$parse:m io.println("parse");$type:m io.println("type");$missing:m io.println(str.concat("missing ";m))}};'

run_case "json.str of a number binds Type" "type" \
'  mt json.str(o;"n"){$ok:v io.println("ok");$err:e mt e{$parse:m io.println("parse");$type:m io.println("type");$missing:m io.println("missing")}};'

run_case "json.str of \"\" is ok" "ok []" \
'  mt json.str(o;"s"){$ok:v io.println(str.concat(str.concat("ok [";v);"]"));$err:e io.println("err")};'

run_case "json.arr of a string binds Type" "type" \
'  mt json.arr(o;"t"){$ok:v io.println("ok");$err:e mt e{$parse:m io.println("parse");$type:m io.println("type");$missing:m io.println("missing")}};'

run_case "json.arr of [] is ok" "ok 0" \
'  mt json.arr(o;"a"){$ok:v io.println(str.concat("ok ";str.fromint(v.len as i64)));$err:e io.println("err")};'

run_case "json.dec let-bound keeps its own slot" "err" \
'  let p1=json.dec("{bad");
  let p2=json.dec("{}");
  mt p1{$ok:v io.println("ok");$err:e io.println("err")};'

# json.get* (127.241): declared T!JsonErr in json.tki, so the payload binds
# through the derivation instead of llvm.c's six-name hand list (E9005 on
# main 8efba9e, where the .tki said plain i64).
run_case "json.getint missing binds Missing + key" "missing q" \
'  mt json.getint(o;"q"){$ok:v io.println("ok");$err:e mt e{$parse:m io.println("parse");$type:m io.println("type");$missing:m io.println(str.concat("missing ";m))}};'

run_case "json.getfloat of a string binds Type" "type" \
'  mt json.getfloat(o;"t"){$ok:v io.println("ok");$err:e mt e{$parse:m io.println("parse");$type:m io.println("type");$missing:m io.println("missing")}};'

run_case "json.getint of 0 after a failure is ok" "ok 0" \
'  mt json.getint(o;"n"){$ok:v io.println(str.concat("ok ";str.fromint(v)));$err:e io.println("err")};'

run_case "str.slice out of range binds SliceErr.msg" "slice index out of range" \
'  io.println(mt str.slice("abc";5;9){$ok:v v;$err:e e.msg});'

run_case "str.slice empty result is ok" "ok []" \
'  io.println(str.concat(str.concat("ok [";mt str.slice("abc";3;3){$ok:v v;$err:e "ERR"});"]"));'

run_case "str.matchbracket binds BracketErr.msg" "not a bracket segment" \
'  io.println(mt str.matchbracket("(ab"){$ok:v v;$err:e e.msg});'

run_case "str.matchbracket ok" "ab" \
'  io.println(mt str.matchbracket("[ab]"){$ok:v v;$err:e e.msg});'

run_case "str.frombytes invalid UTF-8 binds EncodingErr.msg" "invalid UTF-8 encoding" \
'  io.println(mt str.frombytes(@(104;255;105)){$ok:v v;$err:e e.msg});'

run_case "str.frombytes empty is ok" "ok []" \
'  io.println(str.concat(str.concat("ok [";mt str.frombytes(@()){$ok:v v;$err:e "ERR"});"]"));'

run_case "str.toint failure binds ParseErr.msg" "invalid integer" \
'  io.println(mt str.toint("x"){$ok:v "ok";$err:e e.msg});'

run_case "str.toint 0 after a failure is ok" "ok 0" \
'  io.println(mt str.toint("0"){$ok:v str.concat("ok ";str.fromint(v));$err:e e.msg});'

run_case "str.tofloat failure binds ParseErr.msg" "invalid float" \
'  io.println(mt str.tofloat("x"){$ok:v "ok";$err:e e.msg});'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
