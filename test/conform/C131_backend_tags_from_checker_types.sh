#!/usr/bin/env bash
# C131_backend_tags_from_checker_types.sh — codegen's value tags come from the
# checker's types first (story 127.52).
#
# THE CHANGE.  expr_struct_type() in src/llvm.c is the backend's shadow type
# inferencer: from the SHAPE of an expression it guesses the tag codegen
# dispatches on ("$str", "@f64", a struct name, "__map__…").  The checker now
# types most bindings (127.40, 127.114), so expr_struct_type asks Node.rtype
# first (type_backend_tag) and keeps its string inference only for nodes the
# checker left untyped.  That retires 127.28's workaround — a map return
# carried through FnSig.ret_type_name as a "__map__" tag because `let m=mk()`
# used to be untyped — and fixes the one bool shape 127.194 left open.
#
# WHAT THIS DRIVES, each at -O0 and -O2:
#   1. `let m=mk()` for a user function returning a map (str→i64, str→str,
#      i64→i64): .len, .get, .contains and interpolation of a str value — the
#      shapes the "__map__" FnSig tag used to carry.
#   2. a bool field of a struct read off an array element, interpolated
#      (127.194's last case): the element's struct tag is the checker's.
#   3. an f64 array returned by a user function, element interpolated.
#
# Negative control: make expr_struct_type() skip the rtype lookup (return
# expr_struct_type_legacy directly) — case 1 prints map addresses / wrong
# lengths and case 2 prints 1/0.
#
# Story: 127.52

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c131_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C131: backend tags come from the checker's types"
echo "------------------------------------------------"

cat > "${WORK}/t.tk" <<'EOF'
m=c131;
i=io:std.io;
t=$flag{on:bool;n:i64};
f=mk():@(str:i64){<@("k":7;"j":8)};
f=ms():@(str:str){<@("a":"x";"b":"y")};
f=mi():@(i64:i64){<@(1:10;2:20)};
f=fl():@f64{<@(1.5;2.25)};
f=main():i64{
  let m=mk();
  io.println("\(m.len) \(m.get("k")) \(m.contains("j"))");
  let s=ms();
  io.println("\(s.get("b"))");
  let n=mi();
  io.println("\(n.get(2)) \(n.len)");
  let fs=@($flag{on:true;n:1};$flag{on:false;n:2});
  let f0=fs.get(0);
  io.println("\(f0.on) \(fs.get(1).on)");
  let a=fl();
  io.println("\(a.get(1))");
  <0
};
EOF
# 2 entries, k->7, "j" present; b->"y"; 2->20, 2 entries; true false; 2.25
want='2 7 true|y|20 2|true false|2.25|'
for o in -O0 -O2; do
    if "${TKC}" --allow-all "${o}" --out "${WORK}/t" "${WORK}/t.tk" >"${WORK}/t.log" 2>&1; then
        got="$("${WORK}/t" </dev/null 2>&1 | tr '\n' '|')"
        if [ "${got}" = "${want}" ]; then pass "${o}: ${got}"
        else fail "${o}: got [${got}] want [${want}]"; fi
    else
        fail "${o}: did not build: $(awk '/"severity":"error"/' "${WORK}/t.log" | head -1 | cut -c1-240)"
    fi
done

echo ""
echo "C131: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
