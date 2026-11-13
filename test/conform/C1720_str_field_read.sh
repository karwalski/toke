#!/usr/bin/env bash
# C1720_str_field_read.sh -- a field read on a value the checker knows is a
# str (`s.nosuch`) is E4034 at --check, the code the build reports, while the
# str property `.len` and receiver-method calls stay clean (story 127.430).
#
# WHAT WAS WRONG (base ab9d1a7b = fix/z195-mt-get-ok-typed):
#   NODE_FIELD_EXPR typed `.len` on a str and returned TY_UNKNOWN in silence
#   for every other name, so `let s="hey"; s.nosuch` checked clean and the
#   build stopped with codegen's E4034.  When a struct in scope declared a
#   field of that name (`t=$p{name:str}; s.name`), codegen's fallback took
#   that struct's slot: --check and build were clean and the program died
#   with SIGSEGV.  Same for `ss.get(0).nosuch` and, since 127.428, the typed
#   `$ok:v v.nosuch` over a `[str]` lookup.
#
# WHAT IS ASSERTED
#   1. run: `.len`, `.len()`, receiver methods (`split`, `upper`, `trim`,
#      `contains`), a chained `.split(..).len`, a struct's str field's
#      `.len`, and `$ok:v v.len` compile and print the right values
#   2. --check: a field read on a str is E4034 -- a let-bound literal, a
#      name some struct declares, a str parameter, a str struct field, an
#      array `.get`, an `$ok` binding over `[str]`, a no-paren method name,
#      a str-returning call, and inside an interpolation
#   3. the diagnostic is emitted once, carries no `fix`, and names the field
#
# Negative control: C1720_TKC=<path> runs another compiler.  Measured: base
# ab9d1a7b tkc 1 passed, 10 failed (every case in 2 and 3); this branch
# 11 passed.
#
# Story: 127.430
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1720_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1720_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1720: a field read on a str is E4034 at --check (127.430)"
echo "--------------------------------------------------------------------"

run_tk() {  # $1 name, $2 expected stdout; program in $1.tk
    if ! "${TKC}" --check "$1.tk" > "$1.chk" 2>&1; then
        fail "$1: --check"; head -c 400 "$1.chk" | sed 's/^/        /'; echo; return
    fi
    if ! "${TKC}" --out "$1" "$1.tk" > "$1.log" 2>&1; then
        fail "$1: compile"; head -c 400 "$1.log" | sed 's/^/        /'; echo; return
    fi
    got="$(TK_STR_VERIFY=1 "./$1" 2>&1)"
    if [ "${got}" = "$2" ]; then pass "$1: ${2//$'\n'/ | }"
    else fail "$1: got '${got//$'\n'/ | }', want '${2//$'\n'/ | }'"; fi
}
codes() {  # $@ tkc args: the sorted error codes --check reports
    "${TKC}" --check "$@" 2>&1 | grep -o '"error_code":"E[0-9]*"' | cut -d'"' -f4 | sort -u | tr '\n' ' '
}
chk() {  # $1 label, $2 expected codes ("" = clean), rest: tkc args
    local label=$1 want=$2; shift 2
    got="$(codes "$@")"
    if [ "${got}" = "${want}" ]; then pass "--check ${label}: '${got}'"
    else fail "--check ${label}: got '${got}', want '${want}'"; fi
}

cat > val.tk <<'TOKE'
m=val;
i=io:std.io;
t=$pt{x:i64;name:str};
f=main():i64{
  let s="a,bc,d";
  let n=s.len;
  let q=s.len();
  let k=s.split(",").len;
  let u=" Ab ".trim().upper();
  let c=s.contains("bc");
  let p=$pt{x:1;name:"xyz"};
  let ss=@("hey";"you!");
  let w=mt ss.get(1) {$ok:v v.len;$err:e 0};
  io.println("\(n) \(q) \(k) \(u) \(c) \(p.name.len) \(w)");
  <0
};
TOKE
run_tk val "6 6 3 AB true 3 4"

cat > lit.tk <<'TOKE'
m=lit;
i=io:std.io;
f=main():i64{
  let s="hey";
  let z=s.nosuch;
  io.println("\(z)");
  <0
};
TOKE
chk "let-bound literal" "E4034 " lit.tk

cat > decl.tk <<'TOKE'
m=decl;
t=$p{name:str;n:i64};
f=main():i64{
  let s="hey";
  let z=s.name;
  <0
};
TOKE
chk "a name a struct in scope declares" "E4034 " decl.tk

cat > par.tk <<'TOKE'
m=par;
f=hs(a:str):i64{<a.nosuch};
f=main():i64{<hs("x")};
TOKE
chk "str parameter" "E4034 " par.tk

cat > sfld.tk <<'TOKE'
m=sfld;
t=$pt{x:i64;name:str};
f=main():i64{
  let p=$pt{x:1;name:"a"};
  let z=p.name.nosuch;
  <0
};
TOKE
chk "str struct field" "E4034 " sfld.tk

cat > get.tk <<'TOKE'
m=get;
f=main():i64{
  let ss=@("a";"b");
  let z=ss.get(0).nosuch;
  <0
};
TOKE
chk "array .get element" "E4034 " get.tk

cat > okv.tk <<'TOKE'
m=okv;
f=main():i64{
  let ss=@("a";"b");
  let z=mt ss.get(1) {$ok:v v.nosuch;$err:e 0};
  <0
};
TOKE
chk "\$ok binding over [str]" "E4034 " okv.tk

cat > nop.tk <<'TOKE'
m=nop;
f=main():i64{
  let s="ab";
  let u=s.upper;
  <0
};
TOKE
chk "no-paren method name" "E4034 " nop.tk

cat > call.tk <<'TOKE'
m=call;
f=gs():str{<"g"};
f=main():i64{
  let z=gs().nosuch;
  <0
};
TOKE
chk "str-returning call" "E4034 " call.tk

cat > interp.tk <<'TOKE'
m=interp;
i=io:std.io;
f=main():i64{
  let s="hey";
  io.println("\(s.nosuch)");
  <0
};
TOKE
chk "inside an interpolation" "E4034 " interp.tk

# 3. shape: one record, no fix, names the field
out="$("${TKC}" --check lit.tk 2>&1)"
n="$(printf '%s\n' "${out}" | grep -c '"error_code":"E4034"')"
if [ "${n}" = "1" ] && ! printf '%s' "${out}" | grep -q '"fix"' \
   && printf '%s' "${out}" | grep -q '"got":"nosuch"' \
   && printf '%s' "${out}" | grep -q "a str has no field 'nosuch'"; then
    pass "one E4034 record, no fix, got=nosuch"
else
    fail "diagnostic shape: ${out:0:400}"
fi

echo "--------------------------------------------------------------------"
echo "C1720: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
