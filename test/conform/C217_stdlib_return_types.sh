#!/usr/bin/env bash
# C217_stdlib_return_types.sh — a std module call's return is typed by what
# the glue returns (story 127.220 items 2 and 5).
#
# Item 2:
# stdlib/array.tki declared `array.contains` as returning i64 (a gen_tki.py
# ABI placeholder, "gen":"abi"), so the backend read the interface and printed
# the module form as `1`/`0`, while the receiver form `a.contains(3)` printed
# `true`/`false`.  The glue (tk_arr_contains_w) returns 0/1: the return is a
# bool.  gen_tki.py keeps an existing record's hand-written types verbatim, so
# the interface is the source of this type; `gen_tki.py --check` stays clean.
#
# Item 5: every export gen_tki.py generated was typed `i64` on return, the
# ABI slot, including glue that returns a C string (`str.nowunix`,
# `sys.platform`, `file.tempdir`, `str.fromi64`, ... 52 of them), so the
# module form interpolated the POINTER as a number.  gen_stdlib_decls.py's
# scan_str_returns() now establishes from the glue bodies which wrappers
# return a C string on every path, and gen_tki.py corrects a generated
# (`"gen":"abi"`) record's i64 return to str; hand-written returns are never
# touched.  `gen_tki.py --check` fails on a record left i64 (case 5).
#
# Negative control: TKC=<main's toke> TKC_STDLIB_DIR=<main's src/stdlib> (the
# interface is read beside the stdlib dir) -- case 1 prints "1 0|", case 4
# prints pointers.
#
# Not covered here (a separate defect, reported): an UNANNOTATED `let b=` of
# any bool-returning call (`a.contains(3)`, `s.contains(x;"b")`) interpolates
# as 1 on main and on this branch alike.
#
# Story: 127.220 (item 2)

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
run() { rm -f "${WORK}/b"; "${TKC}" --out "${WORK}/b" "$1" > "${WORK}/build.log" 2>&1
        if [ -x "${WORK}/b" ]; then "${WORK}/b" 2>&1 | tr '\n' '|'; else echo "no binary"; fi; }

cat > "${WORK}/mf.tk" <<'EOF'
m=q;
i=io:std.io;
i=arr:std.array;
f=main():i64{
  let a=@(1;2;3);
  io.println("\(arr.contains(a;3)) \(arr.contains(a;9))");
  <0
};
EOF
expect "1 module form interpolates as true/false" "$(run "${WORK}/mf.tk")" "true false|"

cat > "${WORK}/rf.tk" <<'EOF'
m=q;
i=io:std.io;
i=arr:std.array;
f=main():i64{
  let a=@(1;2;3);
  io.println("\(a.contains(3)) \(a.contains(9))");
  if(arr.contains(a;2)){io.println("in")}el{io.println("out")};
  if(arr.contains(a;7)){io.println("in")}el{io.println("out")};
  <0
};
EOF
expect "2 receiver form agrees; the module form still tests as a condition" \
    "$(run "${WORK}/rf.tk")" "true false|in|out|"

"${TKC}" --check "${WORK}/mf.tk" >/dev/null 2>&1
expect "3 --check accepts the module form" "$?" "0"

cat > "${WORK}/sr.tk" <<'EOF'
m=q;
i=io:std.io;
i=s:std.str;
i=sy:std.sys;
f=main():i64{
  io.println("\(s.fromi64(42))! \(s.len(s.nowunix())>5)");
  io.println("[\(sy.platform())]");
  <0
};
EOF
case "$(uname -s)" in Darwin) PLAT=macos ;; Linux) PLAT=linux ;; *) PLAT=unknown ;; esac
expect "4 str-returning glue interpolates as text" "$(run "${WORK}/sr.tk")" "42! true|[${PLAT}]|"

# 5: the generator is the source -- revert one corrected record and --check fails.
CP="${WORK}/repo"; mkdir -p "${CP}/src"
cp -R "${REPO_ROOT}/scripts" "${REPO_ROOT}/stdlib" "${CP}/"
cp -R "${REPO_ROOT}/src/stdlib" "${CP}/src/"
cp "${REPO_ROOT}"/src/*.h "${REPO_ROOT}/src/llvm.c" "${REPO_ROOT}/src/stdlib_deps.c" "${CP}/src/"
python3 - "${CP}/stdlib/sys.tki" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p))
for e in d["exports"]:
    if e.get("name") == "sys.platform": e["return"] = "i64"
open(p, "w").write(json.dumps(d, indent=2) + "\n")
PY
python3 "${CP}/scripts/gen_tki.py" --check > "${WORK}/g.log" 2>&1; rc=$?
expect "5 gen_tki --check rejects a str glue typed i64" \
    "${rc} $(grep -c 'RETURN  sys.platform: i64 -> str' "${WORK}/g.log")" "1 1"

echo "C217: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
