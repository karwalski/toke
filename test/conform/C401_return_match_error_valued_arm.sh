#!/usr/bin/env bash
# C401_return_match_error_valued_arm.sh — `<mt ..` in a `T!$E` function whose
# arms yield the ok value on one path and the ERROR value on another returns
# each path as what it is (story 127.245, found by Batch E2).
#
# THE DEFECT.  The return lowering judged `<expr` by the expression's struct
# type, and a match's struct type came from its error-valued arm, so the whole
# match was an error return: `<mt s.toint("5") {$ok:v v+100;$err:e $e{$bad:1}}`
# stored 105 in @tk_current_error as if it were an error box and the caller
# took `$err`, reading the payload out of address 105 (SIGSEGV here).
# Writing the arm as an early return (`$err:e <$e{..}`) always worked (g4).
# Each error-valued arm now marks a flag at its entry and the return decides
# ok/err on it.
#
# Negative control: TKC from main at 8efba9e — case 1 builds and the program
# dies with SIGSEGV (rc 139), no output.
#
# Story: 127.245

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
run() {
    local o out prev=""
    for o in -O0 -O2; do
        rm -f b
        "${TKC}" --allow-all "${o}" --out b "$1" >/dev/null 2>&1 || { echo "build-failed${o}"; return; }
        out="$(./b 2>&1 | tr '\n' '|'; echo "rc=${PIPESTATUS[0]}")"
        if [ -n "${prev}" ] && [ "${prev}" != "${out}" ]; then echo "O-levels differ: ${prev} / ${out}"; return; fi
        prev="${out}"
    done
    echo "${prev}"
}

# 1. ok and error values from one returned match: struct literal, variant
#    value, `$err(x)`, either arm order, an i64 and a str ok type, and the
#    early-return spelling as the control.
cat > r2.tk <<'EOF'
m=r2;
i=io:std.io;
i=s:std.str;
t=$e{$bad:i64;$msg:str};
f=g(x:str):i64!$e{ <mt s.toint(x) {$ok:v v+100;$err:e $e{$bad:1}} };
f=g2(x:str):i64!$e{ <mt s.toint(x) {$err:e $bad(2);$ok:v v+200} };
f=g3(x:str):str!$e{ <mt s.toint(x) {$ok:v "n\(v)";$err:e $err($msg("m"))} };
f=g4(x:str):i64!$e{ <mt s.toint(x) {$ok:v v;$err:e <$e{$bad:4}} };
f=show(r:i64):str{ <"\(r)" };
f=main():i64{
  let a=mt g("5") {$ok:v v;$err:e mt e {$bad:k 0-k;$msg:m 0-99}};
  let b=mt g("zz") {$ok:v v;$err:e mt e {$bad:k 0-k;$msg:m 0-99}};
  let c=mt g2("5") {$ok:v v;$err:e mt e {$bad:k 0-k;$msg:m 0-99}};
  let d=mt g2("q") {$ok:v v;$err:e mt e {$bad:k 0-k;$msg:m 0-99}};
  let f1=mt g3("7") {$ok:v v;$err:e mt e {$bad:k "bad";$msg:m "msg:\(m)"}};
  let f2=mt g3("x") {$ok:v v;$err:e mt e {$bad:k "bad";$msg:m "msg:\(m)"}};
  let h1=mt g4("0") {$ok:v v;$err:e 0-1};
  let h2=mt g4("x") {$ok:v v;$err:e mt e {$bad:k 0-k;$msg:m 0-99}};
  io.println("\(a) \(b) \(c) \(d) \(f1) \(f2) \(h1) \(h2)");
  <0
};
EOF
expect "1-returned-match-ok-and-err" "$(run r2.tk)" "105 -1 205 -2 n7 msg:m 0 -4|rc=0"

echo "C401: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
