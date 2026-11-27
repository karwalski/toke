#!/usr/bin/env bash
# C1431_emit_interface_func_type.sh -- --emit-interface spells a function
# type in full (story 127.376).
#
# render_type_node (ir.c) had no NODE_FUNC_TYPE case and fell back to the
# head token, so `t=$rec{cb:(i64):i64}` exported {"name":"cb","type":"("},
# and a function-typed parameter or return was "(" too.  A function type is
# now written with the source's spelling: `(P1;P2):R`, each part rendered as
# any other .tki type (bare scalars, `[T]` arrays, records by name).
#
# Importers read the spelling since 127.377 (C1440): an imported
# function-typed field, parameter or return is a function value.  Case 8
# pins that the importing module checks and runs as before.
#
# Negative control: TKC built from integrate/z-wave-1113b (c0a8bee7) --
# cases 1-7 red (each spelled "("); case 8 green on both.
#
# Story: 127.376, 127.559

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/a_lib.tk" <<'EOF'
m=c1431.lib;
t=$pt{x:i64};
t=$rec{n:i64;cb:(i64):i64};
t=$two{f:(i64;str):bool;g:@(i64)};
t=$nest{h:($pt;@(f64)):@(str);k:():i64};
f=inc(x:i64):i64{<x+1};
f=mk():$rec{<$rec{n:1;cb:inc}};
f=ap(h:(i64):i64;x:i64):i64{<h(x)};
f=getf():(i64):i64{<inc};
EOF
( cd "${WORK}" && "${TKC}" --emit-interface --check a_lib.tk >/dev/null 2>&1 )
TKI="${WORK}/c1431.lib.tki"
[ -f "${TKI}" ] || echo "  FAIL: setup: --emit-interface produced no c1431.lib.tki"
has() { grep -cF -- "$1" "${TKI}" 2>/dev/null || true; }
expect "1 record field (i64):i64" "$(has '{"name": "cb", "type": "(i64):i64"}')" "1"
expect "2 two parameters, ';'-separated" "$(has '{"name": "f", "type": "(i64;str):bool"}')" "1"
expect "3 record and array parts" "$(has '{"name": "h", "type": "(pt;[f64]):[str]"}')" "1"
expect "4 no parameters" "$(has '{"name": "k", "type": "():i64"}')" "1"
expect "5 function-typed parameter" "$(has '"name": "ap", "params": ["(i64):i64", "i64"]')" "1"
expect "6 function-typed return" "$(has '"name": "getf", "params": [], "return": "(i64):i64"')" "1"
expect "7 no bare '(' spelling left" "$(has '"("')" "0"
cat > "${WORK}/b_main.tk" <<'EOF'
m=c1431.main;
i=io:std.io;
i=l:c1431.lib;
f=dbl(x:i64):i64{<x*2};
f=main():i64{let r=l.mk();io.println("\(r.n) \(l.ap(dbl;20))");<0};
EOF
out="$(cd "${WORK}" && "${TKC}" --check b_main.tk 2>&1; echo "rc=$?")"
( cd "${WORK}" && tk_build "${TKC}" a_lib.tk b_main.tk -o bm >/dev/null 2>&1 ) || tk_build_failed $?
got="$( [ -x "${WORK}/bm" ] && tk_run "${WORK}/bm" 2>&1 | tr '\n' ' ')"
expect "8 importer checks clean and runs as before" "${out} ${got}" "rc=0 1 40 "

echo "C1431: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
