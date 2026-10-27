#!/usr/bin/env bash
# C760_json_object_surface.sh — std.json's object surface is typed, and a
# document can be read at any depth and built for output (story 113.B.5).
#
# THE DEFECTS.
#   * json.keys / entries / has / haskey / get / set / setint / serialize were
#     reachable but undeclared ("gen":"abi", i64 in and out): no `mt`, no
#     $jsonerr arm, and json.keys of {} or of a non-object was a bare null.
#   * A $jsonval holding a scalar -- what json.at, a json.arr element or a
#     member read hands back -- had no typed read: every reader asked it for
#     a key and answered $missing.
#   * json.entries read every non-string value as "".
#   * json.set / setint pasted the key into the text unescaped (a key holding
#     a quote produced text that is not JSON), moved a replaced member to the
#     end (json_merge), and on any failure returned the input as if written.
#   * A member name written with an escape ("q\"k", "A") was listed by
#     json.keys decoded and then never found by any reader, which compared
#     the raw text.
#
# THE FIX.  json.tki declares the surface (signatures in
# docs/spec/stdlib-signatures.md, held to the .tki by C353); the scalar
# readers take the key "" on a non-object as the value itself (the
# json.arr(doc;"") convention, 127.29); json.get names one member exactly;
# json.newobj + json.set/setint/setf64/setbool/setval build an object through
# json_setmember (order-preserving, key always encoded, result always one
# well-formed object); find_json_key compares decoded names.
#
# WHAT IS ASSERTED.
#   Part 1 (C driver, json.c under ASan+UBSan): json_setmember replace /
#     append / encode / reject; json_member; the "" self-read; escaped names.
#   Part 2 (toke programs): every new or retyped export end to end, its
#     $jsonerr arms, and the --check rejections the declarations now give.
#
# Negative control: C760_STDLIB=<repo>/src/stdlib builds against another
# tree (its .tki is read from <repo>/stdlib).  Measured 2026-10-27:
#   * main's tree (8bb9f63e): 0 passed, 11 failed -- nothing is declared,
#     the C driver does not build, entries prints `s=1 n= o= z=`.
#   * this tree with find_json_key's raw compare and json_locate's ""
#     self-read reverted: 18 passed, 5 failed -- exactly the escaped-name
#     and scalar-read cases in both parts.
#
# Story: 113.B.5
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C760_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c760_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C760: std.json object surface -- typed reads, nested reads, output (113.B.5)"
echo "---------------------------------------------------------------------------"

# ── Part 1: the C core ───────────────────────────────────────────────────
cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
static int set_is(const char *doc, const char *k, const char *v, const char *want) {
    Json j = { doc };
    JsonResult r = json_setmember(j, k, v);
    int ok = !r.is_err && strcmp(r.ok.raw, want) == 0;
    if (!ok) printf("        got [%s]\n", r.is_err ? "(err)" : r.ok.raw);
    if (!r.is_err) free((void *)r.ok.raw);
    return ok;
}
int main(void) {
    CHECK(set_is("{}", "a", "1", "{\"a\":1}"), "setmember on {} appends");
    CHECK(set_is("{\"a\":1,\"b\":2}", "a", "9", "{\"a\":9,\"b\":2}"),
          "setmember replaces a member where it stands");
    CHECK(set_is(" { \"a\" : 1 } ", "c", "\"x\"", "{\"a\" : 1,\"c\":\"x\"}"),
          "setmember appends after the existing members, copied verbatim");
    CHECK(set_is("{}", "q\"k\n", "true", "{\"q\\\"k\\n\":true}"),
          "setmember JSON-encodes the key");
    CHECK(set_is("{\"q\\\"k\":1}", "q\"k", "2", "{\"q\\\"k\":2}"),
          "setmember finds an escaped name by its decoded text");
    JsonResult bad1 = json_setmember((Json){ "[1]" }, "a", "1");
    JsonResult bad2 = json_setmember((Json){ "{}" }, "a", "1 2");
    JsonResult bad3 = json_setmember((Json){ "{}" }, "a", "{\"x\":");
    CHECK(bad1.is_err && bad1.err.kind == JSON_ERR_TYPE, "setmember on a non-object is Type");
    CHECK(bad2.is_err && bad2.err.kind == JSON_ERR_PARSE && bad3.is_err && bad3.err.kind == JSON_ERR_PARSE,
          "setmember refuses a value that is not exactly one JSON value");

    Json d = { "{\"a.b\":{\"c\":7},\"n\":150,\"s\":\"hi\",\"t\":true,\"\":5,\"k\\u0041\":1}" };
    JsonResult m = json_member(d, "a.b");
    CHECK(!m.is_err && strcmp(m.ok.raw, "{\"c\":7}") == 0, "json_member names a key holding '.'");
    if (!m.is_err) free((void *)m.ok.raw);
    JsonResult mm = json_member(d, "zz");
    JsonResult mt = json_member((Json){ "[1]" }, "a");
    CHECK(mm.is_err && mm.err.kind == JSON_ERR_MISSING && mt.is_err && mt.err.kind == JSON_ERR_TYPE,
          "json_member: Missing for no member, Type for a non-object");
    I64JsonResult n = json_i64((Json){ "150" }, "");
    StrJsonResult s = json_str((Json){ " \"h\\ni\" " }, "");
    BoolJsonResult b = json_bool((Json){ "false" }, "");
    F64JsonResult f = json_f64((Json){ "2.5" }, "");
    CHECK(!n.is_err && n.ok == 150 && !s.is_err && strcmp(s.ok, "h\ni") == 0 &&
          !b.is_err && b.ok == 0 && !f.is_err && f.ok == 2.5,
          "key \"\" on a scalar reads the value itself");
    if (!s.is_err) free((void *)s.ok);
    I64JsonResult e = json_i64(d, "");
    CHECK(!e.is_err && e.ok == 5, "key \"\" on an object is still the member named \"\"");
    I64JsonResult ka = json_i64(d, "kA");
    CHECK(!ka.is_err && ka.ok == 1 && json_has(d, "kA"), "an escaped name is found by its decoded text");
    CHECK(json_i64((Json){ "150" }, "x").is_err, "a non-empty key on a scalar is still an error");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o t t.c "${S}/json.c" > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL\|^        got' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing:"
        grep -a 'ERROR: AddressSanitizer\|runtime error' t.out | head -3 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

# ── Part 2: end to end ───────────────────────────────────────────────────
mkdir e2e && cd e2e
run_tk() {   # name expected-output
    local name=$1 want=$2 got
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" --out "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        echo "  FAIL: ${name}.tk did not compile"; sed 's/^/        /' "${name}.log" | head -4; FAIL=$((FAIL + 1)); return
    fi
    got="$(./"${name}" 2>&1)"
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: ${name}: ${want}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${want}', got '${got}'"; FAIL=$((FAIL + 1))
    fi
}
check_rejects() {   # name code description
    local name=$1 code=$2 desc=$3 out rc
    out="$(TKC_STDLIB_DIR="${S}" "${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -ne 0 ] && printf '%s' "${out}" | grep -q "\"error_code\":\"${code}\""; then
        echo "  PASS: ${desc} (${code})"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${desc}: expected ${code}, rc=${rc}: $(printf '%s' "${out}" | head -c 300)"; FAIL=$((FAIL + 1))
    fi
}

cat > keys.tk <<'TOKE'
m=keys;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"a\":1,\"b\":\"x\",\"q\\\"k\":[1,2]}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let ks=mt json.keys(o) { $ok:k k; $jsonerr:e @("ERR") };
  let em=mt json.dec("{}") { $ok:v v; $jsonerr:e o };
  let ke=mt json.keys(em) { $ok:k "len=\(k.len)"; $jsonerr:e "ERR" };
  let ar=mt json.dec("[1]") { $ok:v v; $jsonerr:e o };
  let kt=mt json.keys(ar) { $ok:k "ok"; $jsonerr:e mt e { $type:m "type"; $missing:m "missing"; $parse:m "parse" } };
  io.println("n=\(ks.len) \(ks.get(0)) \(ks.get(2)) empty=\(ke) array=\(kt)");
  <0
};
TOKE
run_tk keys 'n=3 a q"k empty=len=0 array=type'

cat > entries.tk <<'TOKE'
m=entries;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"s\":\"1\",\"n\":1,\"o\":{\"t\":true},\"z\":null}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let es=mt json.entries(o) { $ok:k k; $jsonerr:e @("ERR") };
  io.println("\(es.get(0)) \(es.get(1)) \(es.get(2)) \(es.get(3))");
  <0
};
TOKE
run_tk entries 's="1" n=1 o={"t":true} z=null'

cat > has.tk <<'TOKE'
m=has;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"a\":0,\"k\\u0041\":1}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let a:bool=json.has(o;"a");
  let z:bool=json.haskey(o;"zz");
  let e:bool=json.has(o;"kA");
  io.println("a=\(a) zz=\(z) kA=\(e)");
  <0
};
TOKE
run_tk has "a=true zz=false kA=true"

cat > get.tk <<'TOKE'
m=get;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"a.b\":[1,2],\"n\":150,\"s\":\"hi\",\"cfg\":{\"port\":8080,\"tls\":true}}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let ab=mt json.get(o;"a.b") { $ok:v json.serialize(v); $jsonerr:e "ERR" };
  let nv=mt json.get(o;"n") { $ok:v v; $jsonerr:e o };
  let n=mt json.i64(nv;"") { $ok:v v; $jsonerr:e 0-1 };
  let sv=mt json.get(o;"s") { $ok:v v; $jsonerr:e o };
  let s=mt json.str(sv;"") { $ok:v v; $jsonerr:e "ERR" };
  let pv=mt json.at(o;"cfg.port") { $ok:v v; $jsonerr:e o };
  let port=mt json.u64(pv;"") { $ok:v v; $jsonerr:e 0 };
  let tv=mt json.at(o;"cfg.tls") { $ok:v v; $jsonerr:e o };
  let tls:bool=mt json.bool(tv;"") { $ok:v v; $jsonerr:e false };
  let elems=mt json.arr(o;"a.b") { $ok:v v; $jsonerr:e @() };
  let e1=mt json.i64(elems.get(1);"") { $ok:v v; $jsonerr:e 0-1 };
  let miss=mt json.get(o;"nope") { $ok:v "ok"; $jsonerr:e mt e { $missing:m "missing"; $type:m "type"; $parse:m "parse" } };
  let notobj=mt json.get(nv;"x") { $ok:v "ok"; $jsonerr:e mt e { $missing:m "missing"; $type:m "type"; $parse:m "parse" } };
  let bad=mt json.i64(nv;"x") { $ok:v "ok"; $jsonerr:e "err" };
  io.println("ab=\(ab) n=\(n) s=\(s) port=\(port) tls=\(tls) e1=\(e1) miss=\(miss) notobj=\(notobj) bad=\(bad)");
  <0
};
TOKE
run_tk get "ab=[1,2] n=150 s=hi port=8080 tls=true e1=2 miss=missing notobj=type bad=err"

cat > build.tk <<'TOKE'
m=build;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=json.newobj();
  let o1=mt json.set(o;"name";"al\"ice\n") { $ok:v v; $jsonerr:e o };
  let o2=mt json.setint(o1;"age";0-30) { $ok:v v; $jsonerr:e o1 };
  let o3=mt json.setf64(o2;"pi";3.25) { $ok:v v; $jsonerr:e o2 };
  let o4=mt json.setbool(o3;"ok";false) { $ok:v v; $jsonerr:e o3 };
  let sub=mt json.dec("[1,{\"x\":2}]") { $ok:v v; $jsonerr:e o };
  let o5=mt json.setval(o4;"sub";sub) { $ok:v v; $jsonerr:e o4 };
  let o6=mt json.setint(o5;"age";31) { $ok:v v; $jsonerr:e o5 };
  let o7=mt json.set(o6;"k\"ey";"v") { $ok:v v; $jsonerr:e o6 };
  io.println(json.serialize(o7));
  io.println(json.serialize(o));
  let back=mt json.dec(json.serialize(o7)) { $ok:v v; $jsonerr:e o };
  let nm=mt json.str(back;"name") { $ok:v v; $jsonerr:e "ERR" };
  let kv=mt json.str(back;"k\"ey") { $ok:v v; $jsonerr:e "ERR" };
  let x=mt json.at(back;"sub.1.x") { $ok:v v; $jsonerr:e o };
  let xv=mt json.i64(x;"") { $ok:v v; $jsonerr:e 0-1 };
  io.println("roundtrip name-ok=\(nm=="al\"ice\n") key=\(kv) x=\(xv)");
  <0
};
TOKE
run_tk build '{"name":"al\"ice\n","age":31,"pi":3.25,"ok":false,"sub":[1,{"x":2}],"k\"ey":"v"}
{}
roundtrip name-ok=true key=v x=2'

cat > builderr.tk <<'TOKE'
m=builderr;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=json.newobj();
  let arr=mt json.dec("[1]") { $ok:v v; $jsonerr:e o };
  let nan=0.0/0.0;
  let inf=1.0/0.0;
  let a=mt json.set(arr;"k";"v") { $ok:v "ok"; $jsonerr:e mt e { $missing:m "missing"; $type:m "type"; $parse:m "parse" } };
  let b=mt json.setval(o;"k";$jsonval{raw:"{\"x\":"}) { $ok:v "ok"; $jsonerr:e mt e { $missing:m "missing"; $type:m "type"; $parse:m "parse" } };
  let c=mt json.setf64(o;"k";nan) { $ok:v "ok"; $jsonerr:e mt e { $missing:m "missing"; $type:m "type"; $parse:m "parse" } };
  let d=mt json.setf64(o;"k";inf) { $ok:v "ok"; $jsonerr:e mt e { $missing:m "missing"; $type:m "type"; $parse:m "parse" } };
  let g=mt json.setval(o;"k";arr) { $ok:v json.serialize(v); $jsonerr:e "err" };
  io.println("set-on-array=\(a) bad-val=\(b) nan=\(c) inf=\(d) good=\(g) o=\(json.serialize(o))");
  <0
};
TOKE
run_tk builderr 'set-on-array=type bad-val=parse nan=type inf=type good={"k":[1]} o={}'

# The declarations: an argument of the wrong type is refused at --check.
printf '%s\n' 'm=r1;i=json:std.json;f=main():i64{let k=json.keys("{}");<0};' > r1.tk
check_rejects r1 E4031 "json.keys of a str is refused (it declares \$jsonval)"
printf '%s\n' 'm=r2;i=json:std.json;f=main():i64{let o=json.newobj();let p=json.set(o;"a";5);<0};' > r2.tk
check_rejects r2 E4031 "json.set with an i64 value is refused (it declares str)"
printf '%s\n' 'm=r3;i=json:std.json;f=main():i64{let o=json.newobj();let p=json.setval(o;"a";"x");<0};' > r3.tk
check_rejects r3 E4031 "json.setval with a str value is refused (it declares \$jsonval)"
printf '%s\n' 'm=r4;i=json:std.json;f=main():i64{let o=json.newobj();let p=json.has(o;7);<0};' > r4.tk
check_rejects r4 E4031 "json.has with an i64 key is refused (it declares str)"

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
