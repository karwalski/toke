#!/usr/bin/env bash
# C1830_std_errunion_value_use.sh — a std call whose interface declares `T!E`
# is an error union to the checker, judged as a user `T!E` and a map lookup
# (127.244) are: a use of the result as a plain `T` is E4031 (story 127.319,
# owner decision 127.30 "return error unions", register 1.1 (a) 2026-11-15).
#
# THE DEFECT (main 685aa852, v3.2.0).  The runtime reports these failures on
# the error slot (127.30, 127.241), but the checker typed every std `T!E`
# call TY_UNKNOWN (127.66), so `str.toint("x")+1`, `str.parsefloat(s)*2.0`,
# `os.write(1;s)<=0` (ooke run.tk) and `file.size(p)>0` checked clean and
# evaluated the zero filler on a failure.  183 functions across 37 .tki.
#
# THE FIX.  types.c std_slot_union gives such a call a TY_ERROR_TYPE `T!E`
# carrying slot_lower = TY_UNKNOWN, which infer() stores as the node's rtype,
# so every accepted program lowers exactly as before (the 127.244 mechanism).
# `let` and `mut` bindings of the call bind the union, as for a user `T!E`.
# No `fix` on any of these (owner, 1.1): `mt`, `!` and `getor` are all
# repairs, and a filler for the `$err` arm is never suggested.
#
# One representative per fallible family: parseerr (str.toint,
# str.parsefloat), sliceerr (str.slice), encodingerr (str.frombytes,
# encoding.urldecode), jsonerr (json.i64, json.str), fileerr (file.size,
# file.read), oserr (os.write), argserr (args.get), enverr (env.get),
# tomlerr (toml.i64).  And one per structurally distinct spelling of T!E,
# each asserting the exact machine-readable `got`: a record T (db.one,
# `dbrow!dberr`), a compound T (db.many, `[dbrow]!dberr`), a dotted error
# type (llmtool.chatwithtools, `toolcall!llm.llmerr`) and the sigil spelling
# (tpl.render, `$str!$tmplerr`), whose T `got` named '?' until std_union_of
# read it (127.319 wave 2).
#
# Negative control: C1830_TKC=<v3.2.0 tkc> -- every `rejects` case fails
# (rc 0, clean); every `clean` and `runs` case passes.
#
# Story: 127.319
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1830_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1830_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C1830: a std call declaring T!E is judged as the error union (127.319)"
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

H='i=io:std.io;i=str:std.str;i=json:std.json;i=file:std.file;i=os:std.os;i=args:std.args;i=env:std.env;i=enc:std.encoding;i=toml:std.toml;t=$e{msg:$str};'
O='let o=mt json.dec("{\"a\":1}"){$ok:v v;$jsonerr:e json.newobj()};'

# ── each family, its result read as a plain T ────────────────────────────
rejects p1 'str.toint(..)+1 (parseerr)' "i64!\$parseerr" \
  "m=p1;${H}f=main():i64{let x=str.toint(\"3\")+1;<x};"
rejects p2 'let-bound str.toint, then n*2' "i64!\$parseerr" \
  "m=p2;${H}f=main():i64{let n=str.toint(\"3\");<n*2};"
rejects p3 'a mut binding of str.toint read as i64' "i64!\$parseerr" \
  "m=p3;${H}f=main():i64{let n=mut.str.toint(\"3\");let y=n+1;<y};"
rejects p4 'str.parsefloat(..)*2.0 (parseerr, f64)' "f64!\$parseerr" \
  "m=p4;${H}f=main():i64{let x=str.parsefloat(\"1.5\")*2.0;<0};"
rejects s1 'str.slice into a str parameter (sliceerr)' "str!\$sliceerr" \
  "m=s1;${H}f=h(a:\$str):i64{<str.len(a) as i64};f=main():i64{<h(str.slice(\"abc\";0;1))};"
rejects s2 'str.slice compared as a str' "str!\$sliceerr" \
  "m=s2;${H}f=main():i64{let c=str.slice(\"abc\";0;1);if(c==\"a\"){<1};<0};"
rejects e1 'str.frombytes returned as a str (encodingerr)' "str!\$encodingerr" \
  "m=e1;${H}f=k(b:@(byte)):\$str{<str.frombytes(b)};f=main():i64{<0};"
rejects e2 'encoding.urldecode returned as a str' "str!\$encodingerr" \
  "m=e2;${H}f=k(s:\$str):\$str{<enc.urldecode(s)};f=main():i64{<0};"
rejects j1 'json.i64(..)+1 (jsonerr)' "i64!\$jsonerr" \
  "m=j1;${H}f=main():i64{${O}let n=json.i64(o;\"a\")+1;<n};"
rejects j2 'json.str into an annotated str let' "str!\$jsonerr" \
  "m=j2;${H}f=main():i64{${O}let s:\$str=json.str(o;\"a\");<0};"
rejects f1 'file.size(..)>0 (fileerr)' "i64!\$fileerr" \
  "m=f1;${H}f=main():i64{if(file.size(\"/nonexistent\")>0){<1};<0};"
rejects o1 'os.write(..)<=0 (oserr, the ooke run.tk shape)' "i64!\$oserr" \
  "m=o1;${H}f=main():i64{let n=os.write(1;\"x\");if(n<=0){<1};<0};"
rejects a1 'args.get returned as a str (argserr)' "str!\$argserr" \
  "m=a1;${H}f=k():\$str{<args.get(0)};f=main():i64{<0};"
rejects v1 'env.get returned as a str (enverr)' "str!\$enverr" \
  "m=v1;${H}f=k():\$str{<env.get(\"HOME\")};f=main():i64{<0};"
rejects t1 'toml.i64(..)+1 (tomlerr)' "i64!\$tomlerr" \
  "m=t1;${H}f=main():i64{let d=mt toml.load(\"a=1\"){\$ok:v v;\$err:e <1};let x=toml.i64(d;\"a\")+1;<x};"
# ── each spelling of T!E: `got` names T exactly ─────────────────────────
rejects d1 'db.one returned as a $dbrow (a record T)' '"got":"dbrow!$dberr"' \
  'm=d1;i=db:std.db;f=k():$dbrow{<db.one("select 1";@())};f=main():i64{<0};'
rejects d2 'db.many returned as an @($dbrow) (a compound T)' '"got":"@(dbrow)!$dberr"' \
  'm=d2;i=db:std.db;f=k():@($dbrow){<db.many("select 1";@())};f=main():i64{<0};'
rejects l1 'llmtool.chatwithtools returned as a $toolcall (a dotted error type)' '"got":"toolcall!$llm.llmerr"' \
  'm=l1;i=llm:std.llm;i=lt:std.llmtool;f=k(c:$llmclient):$toolcall{<lt.chatwithtools(c;@())};f=main():i64{<0};'
rejects g1 'tpl.render returned as a $str (the sigil spelling; got names T)' '"got":"str!$tmplerr"' \
  'm=g1;i=tpl:std.template;f=k(t:$tmpl;v:$tmplvars):$str{<tpl.render(t;v)};f=main():i64{<0};'

# ── the union consumed: clean, as before ─────────────────────────────────
clean c1 'mt with $ok/$err arms' \
  "m=c1;${H}f=main():i64{let n=mt str.toint(\"3\"){\$ok:v v;\$err:e 0};<n};"
clean c2 '! in a fallible function' \
  "m=c2;${H}f=k(s:\$str):i64!\$e{<str.toint(s)!\$e+1};f=main():i64{<0};"
clean c3 'mt with the typed error arm ($jsonerr)' \
  "m=c3;${H}f=main():i64{${O}let n=mt json.i64(o;\"a\"){\$ok:v v;\$jsonerr:e 0};<n};"
clean c4 'a statement-position call reads no value' \
  "m=c4;${H}f=main():i64{file.write(\"/tmp/c1830_never\";\"\");file.delete(\"/tmp/c1830_never\");<0};"
clean c5 'a let-bound union matched later' \
  "m=c5;${H}f=main():i64{let r=str.slice(\"abc\";0;1);let s=mt r{\$ok:v v;\$err:e \"\"};io.println(s);<0};"
clean c6 'a mut binding later ASSIGNED a call and matched (135.16a, C067)' \
  "m=c6;${H}f=main():i64{let r=mut.0;r=str.toint(\"0\");io.println(mt r{\$ok:v \"ok|\\(v)\";\$err:e \"err\"});<0};"
clean c7 'a void!E std call in tail position is a void call' \
  "m=c7;i=sse:std.sse;f=j(c:i64):i64{if(c>0){<1};sse.emitdata(c;\"x\")};f=main():i64{<0};"

# ── and lowers as it did ─────────────────────────────────────────────────
runs r1 'mt on str.toint / str.slice: ok and err values' '42 -1 b E ' \
  "m=r1;${H}
f=main():i64{
  io.println(\"\\(mt str.toint(\"42\"){\$ok:v v;\$err:e 0-1}) \\(mt str.toint(\"x\"){\$ok:v v;\$err:e 0-1})\");
  io.println(mt str.slice(\"abc\";1;2){\$ok:v v;\$err:e \"E\"});
  io.println(mt str.slice(\"abc\";2;9){\$ok:v v;\$err:e \"E\"});
  <0};"

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
