#!/usr/bin/env bash
# C068_check_reads_io_tki.sh — `tkc --check` reads stdlib/io.tki, the same
# file codegen reads (story 127.165, whose premise this refutes and pins).
#
# THE REPORT. "llvm.c:1010 reads the io interface; the check path does not, so
# an edit to io.tki alone is invisible to --check."
#
# WHAT WAS MEASURED (d25ba32). resolve_names (names.c, the std.* loop after
# "Register sub-namespace prefixes") opens TKC_STDLIB_DIR/../../stdlib/<m>.tki
# for every std import — io included since 137.12 generated io.tki — which is
# the path llvm.c's register_tki_struct_types / load_stdlib_tki open. What
# --check does NOT do is enforce a std.* interface's FUNCTION records against
# calls (types.c check_tki_call: "if (is_std) return;"), for every std module
# alike and by design — io.tki's parameter types are glue ABI types tagged
# "gen":"abi" (an i64 where the toke type is str), and arity already comes
# from the glue table itself.
#
# WHAT IS ASSERTED, against a stdlib dir that holds only io.tki:
#   1. the shipped io.tki: a type it does not declare is E3011 under --check
#   2. the same io.tki plus one type record: --check resolves that type, and
#      a field access on it is checked (E4025 on a field it lacks)
# so an interface-only edit IS visible to --check.
#
# Negative control: delete the names.c loop's fopen (or point it elsewhere)
# and 2 turns E3011 — red.
#
# Story: 127.165

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
mkdir -p "${WORK}/root/src/stdlib" "${WORK}/root/stdlib"
cp "${REPO_ROOT}/stdlib/io.tki" "${WORK}/root/stdlib/io.tki"
# 127.179: tkc recognises a stdlib directory by its tk_runtime.c (E9022
# otherwise), so the private root must carry it for this test to reach
# the interface loading it is about.
cp "${REPO_ROOT}/src/stdlib/tk_runtime.c" "${WORK}/root/src/stdlib/tk_runtime.c"
errs() { TKC_STDLIB_DIR="${WORK}/root/src/stdlib" "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }

cat > "${WORK}/p.tk" <<'EOF'
m=t;
i=io:std.io;
f=mk(x:$iorec):i64{<x.a};
f=main():i64{
  io.println("hi");
  <0
};
EOF
cat > "${WORK}/q.tk" <<'EOF'
m=t;
i=io:std.io;
f=mk(x:$iorec):i64{<x.nosuch};
f=main():i64{
  <0
};
EOF
expect "1-shipped-io.tki-lacks-the-type" "$(errs "${WORK}/p.tk")" "E3011 "

python3 - "${WORK}/root/stdlib/io.tki" <<'PYEOF'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
d["exports"].append({"kind": "type", "name": "iorec",
                     "fields": [{"name": "a", "type": "i64"}]})
json.dump(d, open(p, "w"), indent=2)
PYEOF
expect "2-edited-io.tki-type-resolves" "$(errs "${WORK}/p.tk")" ""
expect "2-edited-io.tki-field-checked" "$(errs "${WORK}/q.tk")" "E4025 "

echo "C068: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
