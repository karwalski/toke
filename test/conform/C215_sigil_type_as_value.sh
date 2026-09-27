#!/usr/bin/env bash
# C215_sigil_type_as_value.sh — a `$`-type standing where a value is consumed
# is E4033 (story 127.227 item 1).
#
# THE DEFECT, on main at c8575da: `let x=$str;` and `let y=$pt;` checked
# clean, and so did `c=$i64;` and `<$i64` in a function returning $i64 only
# as far as E4031 "expected 'i64', got 'void'"; `h($i64)` said the same.  A
# type node has no value, so the emitter lowered whatever it lowers to.  The
# bare spelling `let x=str;` has been E4033 since 127.98 (i) (names.c); this
# is its sigil counterpart, in types.c: the initialiser of a let, the right
# side of an assignment, a returned value and a call argument.
#
# No fix: a literal, a binding or a typed-empty `@($str)` are all plausible.
# Controls: typed-empty literals, annotations, casts and variant
# constructors keep their type nodes and stay clean (case 6).
#
# Negative control: TKC=<main's toke> -- cases 1-5 fail.
#
# Story: 127.227 (item 1)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code@line, and whether any record carries a fix
diags() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){c=substr($0,RSTART+14,RLENGTH-15); match($0,/"line":[0-9]+/); printf "%s@%s ",c,substr($0,RSTART+7,RLENGTH-7)}'; }
hasfix() { "${TKC}" --check "$1" 2>&1 | awk '/"fix":/{f=1} END{print f?"fix":"nofix"}'; }

cat > "${WORK}/p.tk" <<'EOF'
m=q;
t=$pt{x:i64};
f=h(a:i64):i64{<a};
f=r():i64{<$i64};
f=main():i64{
  let x=$str;
  let y=$pt;
  let c=mut.0;
  c=$i64;
  let z=h($i64);
  <0
};
EOF
D="$(diags "${WORK}/p.tk")"
has() { case " ${D}" in *" $1 "*) echo yes;; *) echo no;; esac; }
expect "1 let x=\$str is E4033"          "$(has E4033@6)" "yes"
expect "2 let y=\$pt (a user type) is E4033" "$(has E4033@7)" "yes"
expect "3 c=\$i64 is E4033"              "$(has E4033@9)" "yes"
expect "4 h(\$i64) is E4033"             "$(has E4033@10)" "yes"
expect "5 <\$i64 is E4033"               "$(has E4033@4)" "yes"
expect "5b no E4031 'got void' remains"  "$(printf '%s' "${D}" | awk '/E4031/{print "E4031"; f=1} END{if(!f) print "none"}')" "none"
expect "5c no fix is offered"            "$(hasfix "${WORK}/p.tk")" "nofix"
msg="$("${TKC}" --check "${WORK}/p.tk" 2>&1 | awk 'match($0,/"message":"[^"]*"/){m=substr($0,RSTART+11,RLENGTH-12); if (index(m,"$str")) {print m; exit}}')"
expect "5d the message names the type"  "${msg}" "'\$str' is a type, not a value"

cat > "${WORK}/ok.tk" <<'EOF'
m=q;
t=$pt{x:i64};
t=$res{ok:i64;bad:str};
f=mk(n:i64):$res{<$ok(n)};
f=main():i64{
  let a:$str="s";
  let e=@($str);
  let m=@($str:$i64);
  let p=$pt{x:1};
  let n=p.x as $f64;
  let r=mk(2);
  <0
};
EOF
expect "6 control: type nodes in type positions stay clean" "$(diags "${WORK}/ok.tk")" ""

echo "C215: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
