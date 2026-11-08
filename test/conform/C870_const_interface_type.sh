#!/usr/bin/env bash
# C870_const_interface_type.sh -- a module constant's written type is its
# declared type, in both the .tki interface and the companion (story 127.311).
#
# THE DEFECT.  NODE_CONST_DECL children are [0]=name, [1]=literal, [2]=type
# (parser.c parse_const_decl).  The .tki writer (ir.c emit_interface) and the
# companion's constants table (companion.c emit_constants_section) both read
# children[1] as the type.  ir.c also required that child to be a
# NODE_TYPE_EXPR, which a literal never is, so every const record was written
# `"type": "unknown"`; the companion printed the literal in its Type column
# (`| lim | 5 | lim (5) |`).
#
# THE FIX.  Both writers read children[2].  Nothing else changes: whether a
# .tki const record should also carry the value, and whether constants are
# exported across modules at all, is the 127.310 owner decision, so this test
# asserts the type only.
#
# WHAT IS ASSERTED, for i64 / str / f64 / bool / u32 constants:
#   1. `--emit-interface` writes each const record with its declared type;
#   2. no const record says "unknown";
#   3. the func record beside them is unchanged;
#   4. the companion's Constants table shows the declared type, not the
#      literal.
#
# Negative control: C870_TKC=<binary> runs the same checks with another
# compiler.  With the integrate/z-wave-1108 binary (9224b22f, before this
# fix): 2 passed, 11 failed.  On this branch: 13 passed.
#
# Story: 127.311
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C870_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c870_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C870: a module constant's .tki / companion type is its declared type (127.311)"
echo "----------------------------------------------------------------------"

cat > lib.tk <<'TOKE'
m=lib;
lim=5:i64;
name="x":str;
ratio=1.5:f64;
ok=true:bool;
small=7:u32;
f=get():i64{<lim};
TOKE

if "${TKC}" --emit-interface --check lib.tk >emit.out 2>&1 && [ -f lib.tki ]; then
    pass "--emit-interface --check writes lib.tki"
else
    fail "--emit-interface --check did not write lib.tki: $(head -c 300 emit.out)"
    lib_missing=1
fi

# 1. each const record carries its declared type
for pair in lim:i64 name:str ratio:f64 ok:bool small:u32; do
    n="${pair%%:*}"; t="${pair#*:}"
    want="{\"kind\": \"const\", \"name\": \"${n}\", \"type\": \"${t}\"}"
    if [ -z "${lib_missing:-}" ] && grep -qF "${want}" lib.tki; then
        pass ".tki const ${n} has type ${t}"
    else
        fail ".tki const ${n}: want ${want}; got: $(grep -F "\"name\": \"${n}\"" lib.tki 2>/dev/null)"
    fi
done

# 2. no const record is "unknown"
if [ -z "${lib_missing:-}" ] && ! grep -q '"kind": "const".*"unknown"' lib.tki; then
    pass "no const record is typed \"unknown\""
else
    fail "a const record is typed \"unknown\""
fi

# 3. the func record is unchanged
if [ -z "${lib_missing:-}" ] && grep -qF '{"kind": "func", "name": "get", "params": [], "return": "i64"}' lib.tki; then
    pass "func record beside the consts is unchanged"
else
    fail "func record changed: $(grep -F '"get"' lib.tki 2>/dev/null)"
fi

# 4. the companion's Constants table shows the declared type
"${TKC}" --companion lib.tk >comp.md 2>comp.err
for row in '| `lim` | `i64` |' '| `name` | `str` |' '| `ratio` | `f64` |' \
           '| `ok` | `bool` |' '| `small` | `u32` |'; do
    if grep -qF "${row}" comp.md; then
        pass "companion row ${row}"
    else
        n="$(printf '%s' "${row}" | cut -d'`' -f2)"
        fail "companion row: want ${row}; got: $(grep -F "| \`${n}\` |" comp.md)"
    fi
done

echo "----------------------------------------------------------------------"
echo "C870: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
