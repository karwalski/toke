#!/usr/bin/env bash
# C707 -- a json.parse result used as a JSON document is a compile error
# (story 138.9, loke T-17; owner decision docs/owner-decisions-2026-10-01.md
# §2.9 (b)).
#
# json.parse (tk_json_parse) reads a str, number, bool or array and returns
# that value, never a document.  loke's T-17 repro bound `json.parse(<object
# text>)` and read a field with json.getstr: the glue read a string pointer
# as a `Json *`, the program printed `parse a=[]` beside `decode a=[xyz]` and
# exited 0.  The checker now refuses a json.parse call -- or a name an
# un-annotated `let` bound to one -- as the first argument of a std.json
# member that reads a document (E4031, no fix).  json.parse's own API is
# unchanged (C255, C242).
#
# Negative control: TKC=<c7f376a7's toke> -- cases 1-4 fail (rc=0); the
# controls 5-7 and the list check 8 pass there too.
#
# Story: 138.9

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c707_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT

# "rc=N codes@line:col" for --check of a source file
check() {
    "${TKC}" --check "$1" > "${WORK}/o" 2>&1; local rc=$?
    local d
    d=$(python3 - "${WORK}/o" <<'PY'
import json, sys
out = []
for line in open(sys.argv[1]):
    line = line.strip()
    if line.startswith('{'):
        r = json.loads(line)
        out.append('%s@%d:%d' % (r['error_code'], r['pos']['line'], r['pos']['col']))
print(' '.join(out))
PY
)
    echo "rc=${rc} ${d}"
}

# 1: loke's T-17 repro, verbatim in shape.
cat > "${WORK}/t17.tk" <<'TOKE'
m=s17;
i=io:std.io;
i=json:std.json;
f=main():i64{
  let d1=json.decode("{\"a\":\"xyz\"}");
  let s1:str=json.getstr(d1;"a");
  io.println("decode a=[\(s1)]");
  let d2=json.parse("{\"a\":\"xyz\"}");
  let s2:str=json.getstr(d2;"a");
  io.println("parse  a=[\(s2)]");
  <0
};
TOKE
expect "1 loke T-17: json.getstr on a json.parse result is E4031 at the argument" \
    "$(check "${WORK}/t17.tk")" "rc=1 E4031@9:26"
expect "1b ... naming json.decode" "$(grep -c 'decode the text with json.decode' "${WORK}/o")" "1"

# 2: the call passed directly, under another alias.
printf '%s\n' 'm=c;i=j:std.json;f=main():i64{let n=j.getint(j.parse("{\"a\":1}");"a");<0};' > "${WORK}/direct.tk"
r="$(check "${WORK}/direct.tk")"; r="${r%%@*}"
expect "2 j.getint(j.parse(..);..) is E4031" "${r}" "rc=1 E4031"

# 3: a typed member (json.str takes a Json).
printf '%s\n' 'm=c;i=json:std.json;f=main():i64{let p=json.parse("{}");let s=json.str(p;"a");<0};' > "${WORK}/typed.tk"
r="$(check "${WORK}/typed.tk")"; r="${r%%@*}"
expect "3 json.str on a json.parse result is E4031" "${r}" "rc=1 E4031"

# 4: keys / has / len read a document too.
printf '%s\n' 'm=c;i=json:std.json;f=main():i64{let p=json.parse("{}");let k=json.keys(p);<0};' > "${WORK}/keys.tk"
r="$(check "${WORK}/keys.tk")"; r="${r%%@*}"
expect "4 json.keys on a json.parse result is E4031" "${r}" "rc=1 E4031"

# 5: the decode path builds and reads the field.
cat > "${WORK}/dec.tk" <<'TOKE'
m=c;i=io:std.io;i=json:std.json;
f=main():i64{let d=json.decode("{\"a\":\"xyz\"}");let s:str=json.getstr(d;"a");io.println("decode a=[\(s)]");<0};
TOKE
"${TKC}" --allow-all --out "${WORK}/dec" "${WORK}/dec.tk" > "${WORK}/o" 2>&1
expect "5 json.decode + json.getstr still builds and reads the field" \
    "$("${WORK}/dec" 2>&1)" "decode a=[xyz]"

# 6: json.parse's own uses are untouched (C255 / C242 shapes).
printf '%s\n' 'm=main;i=j:std.json;i=s:std.str;f=main():i64{let t:str=j.parse(s.argv(1));j.print(t);let x=j.parse(s.argv(1));j.print(x.len);<0};' > "${WORK}/own.tk"
expect "6 annotated and un-annotated json.parse results printed: clean" \
    "$(check "${WORK}/own.tk")" "rc=0 "

# 7: an annotated binding is not traced (the program states its type) -- but
# since 138.46 its stated type is judged: an i64 where json.has declares a
# $jsonval record is E4031 by type (one diagnostic, at the argument), not by
# this file's json.parse trace.
printf '%s\n' 'm=c;i=json:std.json;f=main():i64{let p:i64=json.parse("7");let h=json.has(p;"a");<0};' > "${WORK}/ann.tk"
expect "7 an annotated json.parse binding is judged by its type (138.46), not traced" "$(check "${WORK}/ann.tk")" "rc=1 E4031@1:75"

# 8: the checker's list is exactly the json_glue.c wrappers that read their
# first argument as a Json * (directly or through another wrapper).
expect "8 JSON_DOC_READERS (src/types.c) matches the glue" "$(python3 - "${REPO_ROOT}" <<'PY'
import json, re, sys
root = sys.argv[1]
src = open(root + '/src/stdlib/json_glue.c').read()
fns = {}
for m in re.finditer(r'int64_t tk_json_(\w+)_w\((?:int64_t (\w+))?[^)]*\)\s*\{', src):
    i, depth = m.end(), 1
    while depth:
        depth += (src[i] == '{') - (src[i] == '}'); i += 1
    fns[m.group(1)] = (m.group(2), src[m.end():i])
def reads(name, seen=()):
    if name not in fns or not fns[name][0]: return False
    p, body = fns[name]
    if re.search(r'\(Json\s*\*\)\s*\(intptr_t\)\s*%s\b' % p, body): return True
    return any(d != name and d not in seen and reads(d, seen + (name,))
               for d in re.findall(r'tk_json_(\w+)_w\(\s*%s\b' % p, body))
tki = json.load(open(root + '/stdlib/json.tki'))
want = sorted(e['name'][5:] for e in tki['exports'] if e.get('kind') == 'func' and reads(e['name'][5:]))
t = open(root + '/src/types.c').read()
blk = re.search(r'JSON_DOC_READERS\[\] = \{(.*?)\};', t, re.S)
have = sorted(re.findall(r'"(\w+)"', blk.group(1))) if blk else []
print('same' if want == have else 'glue %s / checker %s' % (want, have))
PY
)" "same"

echo "C707: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
