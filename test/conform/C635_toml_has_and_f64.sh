#!/usr/bin/env bash
# C635_toml_has_and_f64.sh — std.toml gains toml.has(v; key):bool and
# toml.f64(v; key):f64!$tomlerr (121.68).
#
# THE GAP.  std.toml had str/i64/bool/section accessors only.  A float-valued
# key could not be read at all, and a key that is absent could not be told
# from one whose value is an array or a table: every accessor fails on both.
# ooke therefore could not detect a float- or array-valued config key.
#
# Document (one per program run):
#   ratio = 1.5 / zero = 0.0 / n = 3 / name = "x" / on = true / list = [1, 2]
#   [server]  scale = -2.25     [server.tls]  on = true
# Cases:
#   1 toml.f64(cfg;"ratio")          -> 1.5
#   2 toml.f64(cfg;"zero")           -> $ok 0 (0.0's bits are the 0 sentinel:
#                                       only the error slot can say ok)
#   3 toml.f64(cfg;"missing")        -> $err "key not found"
#   4 toml.f64(cfg;"name") (string)  -> $err "key is not a float"
#   5 toml.f64(cfg;"on")   (bool)    -> $err "key is not a float"
#   6 toml.f64(cfg;"server") (table) -> $err "key is not a float"
#   7 toml.f64(cfg;"n")    (integer) -> 3 (a decimal integer is read as f64;
#                                       documented)
#   8 nested: toml.f64(section(cfg;"server");"scale") * 2.0 -> -4.5 (a real
#     f64: arithmetic on it is floating point)
#   9 toml.has(cfg;"list")  (array)  -> true
#  10 toml.has(cfg;"server") (table) -> true
#  11 toml.has(cfg;"missing")        -> false
#  12 toml.has(cfg;"server.scale")   -> false: keys are not dotted paths
#  13 toml.has(server;"tls")  (nested table in a section) -> true
#  14 toml.has(server;"ratio") (a parent's key)            -> false
#  15 toml.has(cfg;42) -> E4031 at --check (the key parameter is typed str)
#
# Negative control: C635_STDLIB=<main's src/stdlib at 2aa48c88, stdlib/ beside
# it> — neither function is an export there and the program does not compile
# (measured Darwin 25.6).
#
# Story: 121.68
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C635_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'EOF'
m=main;i=io:std.io;i=toml:std.toml;i=s:std.str;
f=yn(b:bool):$str{ if(b){<"true"}; <"false" };
f=main():i64{
  let cfg = mt toml.load("ratio = 1.5\nzero = 0.0\nn = 3\nname = \"x\"\non = true\nlist = [1, 2]\n[server]\nscale = -2.25\n[server.tls]\non = true\n") {$ok:v v;$err:e < 1};
  io.println(mt toml.f64(cfg; "ratio") {$ok:v s.fromfloat(v);$err:e e.msg});
  io.println(mt toml.f64(cfg; "zero") {$ok:v s.concat("ok ";s.fromfloat(v));$err:e e.msg});
  io.println(mt toml.f64(cfg; "missing") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.f64(cfg; "name") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.f64(cfg; "on") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.f64(cfg; "server") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.f64(cfg; "n") {$ok:v s.fromfloat(v);$err:e e.msg});
  let srv = mt toml.section(cfg; "server") {$ok:v v;$err:e < 1};
  io.println(mt toml.f64(srv; "scale") {$ok:v s.fromfloat(v*2.0);$err:e e.msg});
  io.println(yn(toml.has(cfg; "list")));
  io.println(yn(toml.has(cfg; "server")));
  io.println(yn(toml.has(cfg; "missing")));
  io.println(yn(toml.has(cfg; "server.scale")));
  io.println(yn(toml.has(srv; "tls")));
  io.println(yn(toml.has(srv; "ratio")));
  <0
};
EOF
if ! "${TKC}" "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C635: 0 passed, 1 failed"; exit 1
fi
OUT="$("${WORK}/b" 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-f64-float"         "$(line 1)"  "1.5"
expect "2-f64-zero-is-ok"    "$(line 2)"  "ok 0"
expect "3-f64-missing"       "$(line 3)"  "key not found"
expect "4-f64-string"        "$(line 4)"  "key is not a float"
expect "5-f64-bool"          "$(line 5)"  "key is not a float"
expect "6-f64-table"         "$(line 6)"  "key is not a float"
expect "7-f64-integer"       "$(line 7)"  "3"
expect "8-f64-nested-arith"  "$(line 8)"  "-4.5"
expect "9-has-array"         "$(line 9)"  "true"
expect "10-has-table"        "$(line 10)" "true"
expect "11-has-missing"      "$(line 11)" "false"
expect "12-has-not-dotted"   "$(line 12)" "false"
expect "13-has-nested-table" "$(line 13)" "true"
expect "14-has-parent-key"   "$(line 14)" "false"
cat > "${WORK}/q.tk" <<'EOF'
m=main;i=toml:std.toml;
f=main():i64{
  let cfg = mt toml.load("x = 1.0\n") {$ok:v v;$err:e < 1};
  let a = toml.has(cfg; 42);
  <0
};
EOF
"${TKC}" --check "${WORK}/q.tk" > "${WORK}/q.log" 2>&1
expect "15-typed-key-param" "$(grep -o '"error_code":"E4031"' "${WORK}/q.log" | head -1)" '"error_code":"E4031"'
echo "C635: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
