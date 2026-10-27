#!/usr/bin/env bash
# C750_toml_arrays_keys_free.sh — std.toml gains array accessors, keys/len and
# free, and every accessor's error says which failure it is (story 113.B.4).
#
# THE GAPS.  (1) tomlc99 parses arrays, but std.toml exposed none: a config
# list (`plugins = ["a", "b"]`, `[[route]]`) could not be read at all.  (2) A
# table's keys could not be listed or counted.  (3) docs/stdlib/toml.md told
# callers to release a document, and there was no function to do it.  (4)
# toml.str/i64/bool said "key not found or not a string" (etc.) and
# toml.section "section not found", so a typo could not be told from a
# mistyped value.
#
# THE SURFACE (docs/spec/stdlib-signatures.md §std.toml).
#   toml.strarr / i64arr / boolarr / sectionarr (v; key) : @T!$tomlerr
#   toml.keys(v):@$str   toml.len(v):u64   toml.free(v):bool
# toml.free frees a DOCUMENT once; on a section view, an array-of-tables
# element or a document already freed it does nothing and is false.
# (toml.f64arr is withheld until story 127.297: an untyped binding reads the
# double's bits as an integer.)
#
# Document: name="x" n=0 ports=[80,0,443] tags=["a","b"] flags=[true,false]
#   empty=[] mixed=[1,"two"]  [server] host="h"  [[plugin]] name=p1 / p2
# Cases (one output line each):
#   1 i64arr ports: len and the stored 0         -> "3 0"
#   2 strarr tags .get(1)                        -> b
#   3 boolarr flags                              -> "true false"
#   4 strarr of an empty array: $ok, len 0       -> 0
#   5 i64arr of a mixed array                    -> array element 1 is not an integer
#   6 i64arr of an absent key                    -> key not found
#   7 i64arr of a string value                   -> key is not an array
#   8 sectionarr plugin, .get(1) name            -> p2
#   9 sectionarr of a string array               -> array element 0 is not a table
#  10 keys(cfg): values, then arrays, then tables, document order within each
#  11 keys(server)                               -> host,
#  12 len(cfg)                                   -> 9
#  13-18 accessor errors say which: absent vs wrong type (str/i64/bool/section)
#  19-22 free: section false, [[plugin]] element false, cfg true, cfg again false
#  23 a key list taken before free is still readable after it (keys are copies)
#  24 toml.free releases memory: peak RSS of 20,000 load+free is under a
#     quarter of 20,000 loads kept (measured 1.7 MB vs 15.2 MB, Darwin 25.6);
#     SKIP if /usr/bin/time cannot report it.
#  25 toml.strarr(cfg; 42) -> E4031 at --check (the key parameter is $str)
#
# Negative control: C750_STDLIB=<main's src/stdlib at 8bb9f63e, stdlib/ beside
# it> — none of the new functions is exported there and the probe does not
# compile; cases 13-18 read the old "key not found or not ..." messages.
#
# Story: 113.B.4
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C750_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c750_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'EOF'
m=main;i=io:std.io;i=toml:std.toml;i=s:std.str;
f=yn(b:bool):$str{ if(b){<"true"}; <"false" };
f=join(a:@$str):$str{ let out=mut.""; let i=mut.0; lp(i<a.len){ out=s.concat(out; s.concat(a.get(i); ",")); i=i+1 }; <out };
f=main():i64{
  let cfg = mt toml.load("name = \"x\"\nn = 0\nports = [80, 0, 443]\ntags = [\"a\", \"b\"]\nflags = [true, false]\nempty = []\nmixed = [1, \"two\"]\n[server]\nhost = \"h\"\n[[plugin]]\nname = \"p1\"\n[[plugin]]\nname = \"p2\"\n") {$ok:v v;$err:e < 1};
  let p = mt toml.i64arr(cfg; "ports") {$ok:v v;$err:e < 2};
  io.println(s.concat(s.fromint(p.len as i64); s.concat(" "; s.fromint(p.get(1)))));
  let t = mt toml.strarr(cfg; "tags") {$ok:v v;$err:e < 3};
  io.println(t.get(1));
  let b = mt toml.boolarr(cfg; "flags") {$ok:v v;$err:e < 5};
  io.println(s.concat(yn(b.get(0)); s.concat(" "; yn(b.get(1)))));
  io.println(mt toml.strarr(cfg; "empty") {$ok:v s.fromint(v.len as i64);$err:e e.msg});
  io.println(mt toml.i64arr(cfg; "mixed") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.i64arr(cfg; "nope") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.i64arr(cfg; "name") {$ok:v "ok";$err:e e.msg});
  let pl = mt toml.sectionarr(cfg; "plugin") {$ok:v v;$err:e < 6};
  io.println(mt toml.str(pl.get(1); "name") {$ok:v v;$err:e e.msg});
  io.println(mt toml.sectionarr(cfg; "tags") {$ok:v "ok";$err:e e.msg});
  let k = toml.keys(cfg);
  io.println(join(k));
  let srv = mt toml.section(cfg; "server") {$ok:v v;$err:e < 7};
  io.println(join(toml.keys(srv)));
  io.println(s.fromint(toml.len(cfg) as i64));
  io.println(mt toml.str(cfg; "n") {$ok:v v;$err:e e.msg});
  io.println(mt toml.str(cfg; "zz") {$ok:v v;$err:e e.msg});
  io.println(mt toml.i64(cfg; "name") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.bool(cfg; "name") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.section(cfg; "name") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.section(cfg; "zz") {$ok:v "ok";$err:e e.msg});
  io.println(yn(toml.free(srv)));
  io.println(yn(toml.free(pl.get(0))));
  io.println(yn(toml.free(cfg)));
  io.println(yn(toml.free(cfg)));
  io.println(k.get(0));
  <0
};
EOF
if ! "${TKC}" "${WORK}/p.tk" --out "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C750: 0 passed, 1 failed"; exit 1
fi
OUT="$("${WORK}/b" 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-i64arr"               "$(line 1)"  "3 0"
expect "2-strarr"               "$(line 2)"  "b"
expect "3-boolarr"              "$(line 3)"  "true false"
expect "4-empty-array-ok"       "$(line 4)"  "0"
expect "5-mixed-element"        "$(line 5)"  "array element 1 is not an integer"
expect "6-array-absent"         "$(line 6)"  "key not found"
expect "7-array-wrong-type"     "$(line 7)"  "key is not an array"
expect "8-sectionarr"           "$(line 8)"  "p2"
expect "9-sectionarr-not-table" "$(line 9)"  "array element 0 is not a table"
expect "10-keys-order"          "$(line 10)" "name,n,ports,tags,flags,empty,mixed,plugin,server,"
expect "11-keys-section"        "$(line 11)" "host,"
expect "12-len"                 "$(line 12)" "9"
expect "13-str-wrong-type"      "$(line 13)" "key is not a string"
expect "14-str-absent"          "$(line 14)" "key not found"
expect "15-i64-wrong-type"      "$(line 15)" "key is not an integer"
expect "16-bool-wrong-type"     "$(line 16)" "key is not a boolean"
expect "17-section-wrong-type"  "$(line 17)" "key is not a table"
expect "18-section-absent"      "$(line 18)" "key not found"
expect "19-free-section"        "$(line 19)" "false"
expect "20-free-array-element"  "$(line 20)" "false"
expect "21-free-document"       "$(line 21)" "true"
expect "22-free-twice"          "$(line 22)" "false"
expect "23-keys-outlive-free"   "$(line 23)" "name"

# 24: free releases the document.
for mode in free keep; do
    FREE=""; [ "${mode}" = free ] && FREE='let f = toml.free(cfg);'
    cat > "${WORK}/r_${mode}.tk" <<EOF
m=main;i=toml:std.toml;
f=main():i64{
  let src = "a = 1\nb = \"two\"\nc = [1, 2, 3]\n[t]\nx = 1\ny = 2\nz = 3\n";
  let i = mut.0;
  lp(i<20000){
    let cfg = mt toml.load(src) {\$ok:v v;\$err:e < 1};
    ${FREE}
    i = i + 1
  };
  <0
};
EOF
    "${TKC}" "${WORK}/r_${mode}.tk" --out "${WORK}/r_${mode}" > "${WORK}/r.log" 2>&1 \
        || { echo "FAIL 24 build r_${mode}"; head -c 400 "${WORK}/r.log"; FAIL=$((FAIL + 1)); }
done
maxrss() {   # maxrss BIN: peak RSS in bytes, empty if unavailable
    local o
    if o="$(/usr/bin/time -l "$1" 2>&1 >/dev/null)" && printf '%s' "${o}" | grep -q 'maximum resident'; then
        printf '%s\n' "${o}" | awk '/maximum resident/ {print $1}'
    elif o="$(/usr/bin/time -v "$1" 2>&1 >/dev/null)" && printf '%s' "${o}" | grep -q 'Maximum resident'; then
        printf '%s\n' "${o}" | awk -F: '/Maximum resident/ {gsub(/ /,"",$2); print $2 * 1024}'
    fi
}
if [ -x "${WORK}/r_free" ] && [ -x "${WORK}/r_keep" ]; then
    RF="$(maxrss "${WORK}/r_free")"; RK="$(maxrss "${WORK}/r_keep")"
    if [ -z "${RF}" ] || [ -z "${RK}" ]; then
        echo "SKIP 24-free-releases (no /usr/bin/time RSS report)"
    elif [ $((RF * 4)) -lt "${RK}" ]; then
        echo "PASS 24-free-releases (max RSS ${RF} with free, ${RK} without)"; PASS=$((PASS + 1))
    else
        echo "FAIL 24-free-releases (max RSS ${RF} with free, ${RK} without)"; FAIL=$((FAIL + 1))
    fi
fi

cat > "${WORK}/q.tk" <<'EOF'
m=main;i=toml:std.toml;
f=main():i64{
  let cfg = mt toml.load("x = [1]\n") {$ok:v v;$err:e < 1};
  let k = mt toml.strarr(cfg; 42) {$ok:v 0;$err:e 1};
  <0
};
EOF
"${TKC}" --check "${WORK}/q.tk" > "${WORK}/q.log" 2>&1
expect "25-typed-param" "$(grep -o '"error_code":"E4031"' "${WORK}/q.log" | head -1)" '"error_code":"E4031"'
echo "C750: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
