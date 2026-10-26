#!/usr/bin/env bash
# C562_std_member_not_resolved_to_user_fn.sh — a std member's `$err` payload
# is resolved only against std, never against a user function spelled like
# the member (story 127.252 item 3).
#
# THE DEFECT.  classify_fallible_call's same-module fallback
# `lookup_fn(member)` ran for every qualified callee, std included.  An
# extern keeps its bare C name (127.212), so `f=get(..):i64!$myerr;` is a
# FnSig named `get`, and `args.get(k)` / `env.get(k)` (the index spelling,
# 127.249 (3)) or `file.read(p)` with a user `read` took the USER function's
# error type as their payload:
#   - `$err:e e.msg` on args.get read field 1 of ArgsErr as if it were
#     $myerr{code;msg}: main printed "err:" -- a silent wrong value;
#   - a nested `mt e { $notfound.. }` over EnvErr / FileErr passed --check
#     and died in codegen with E9005 ("the matched value is a 'myerr'").
#
# NOW: when the qualifier is a std import alias (and not a value shadowing
# it), neither fallback consults a bare user function name.
#
# Negative control: TKC=<main cde89021's toke> -- cases 1-3 fail.  Case 4
# (a user module's own `get` through its alias) is green on both.
#
# Story: 127.252 (3)
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
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
# run FILE [ARGS]: build, run; stdout then rc, or the first error code
run() { local f="$1"; shift
    if ! "${TKC}" -I . -o prog "${f}" > b.log 2>&1; then
        python3 -c '
import sys, json
for l in open("b.log"):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("severity") == "error": print("build:" + d["error_code"]); break' ; return; fi
    ./prog "$@"; echo "rc=$?"; }

cat > a.tk <<'EOF'
m=ae;
i=io:std.io;
i=args:std.args;
t=$myerr{code:i64;msg:str};
f=get(k:i64):i64!$myerr;
f=main():i64{
  let c=mt args.get(7 as u64) { $ok:v "ok:\(v)"; $err:e "err:\(e.msg)" };
  io.println(c);
  <0
};
EOF
expect "1 args.get with an extern get!\$myerr: e.msg is argserr's" "$(run a.tk)" \
    "err:args.get: index out of bounds
rc=0"

cat > e.tk <<'EOF'
m=ae;
i=io:std.io;
i=env:std.env;
t=$myerr{code:i64;note:str};
f=get(k:str):i64!$myerr;
f=main():i64{
  let a=mt env.get("C562_UNSET_VAR") { $ok:v "ok:\(v)"; $err:e mt e { $notfound:m "nf:\(m)"; $invalid:m "inv" } };
  io.println(a);
  <0
};
EOF
expect "2 env.get with an extern get!\$myerr: e is enverr" "$(unset C562_UNSET_VAR; run e.tk)" \
    "nf:env.get: environment variable not set
rc=0"

cat > r.tk <<'EOF'
m=ae;
i=io:std.io;
i=file:std.file;
t=$myerr{code:i64;msg:str};
f=read(p:str):str!$myerr;
f=main():i64{
  let c=mt file.read("/nonexistent/c562") { $ok:v "ok"; $err:e mt e { $notfound:m "nf"; $permission:m "perm"; $io:m "io"; $invalid:m "inv"; $isdir:m "dir"; $symlink:m "sym" } };
  io.println(c);
  <0
};
EOF
expect "3 file.read (call spelling) with an extern read!\$myerr: e is fileerr" "$(run r.tk --allow-read)" \
    "nf
rc=0"

cat > u.tk <<'EOF'
m=u;
t=$uerr{code:i64;msg:str};
f=get(k:i64):i64!$uerr{ if(k<0){ <$uerr{code:k;msg:"neg"} }; <k };
EOF
"${TKC}" --emit-interface --check u.tk > /dev/null 2>&1
cat > m.tk <<'EOF'
m=mm;
i=io:std.io;
i=u:u;
f=main():i64{
  let c=mt u.get(0-4) { $ok:v "ok:\(v)"; $err:e "err:\(e.code):\(e.msg)" };
  io.println(c);
  <0
};
EOF
rm -f prog
if "${TKC}" --allow-all -I . --out prog m.tk u.tk > b.log 2>&1; then got="$(./prog; echo "rc=$?")"; else got="build-failed"; fi
expect "4 control: a user module's own get via its alias keeps its payload" "${got}" \
    "err:-4:neg
rc=0"

echo "C562: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
