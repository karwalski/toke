#!/usr/bin/env bash
# C112_user_extern_links_to_c.sh — a user extern keeps its C name (story 127.212).
#
# THE DEFECT.  An extern is a function declaration without a body; the spec
# (docs/spec/semantics.md §7.2, v0.3 §24.2.1) says it is implemented outside
# toke and resolved by the linker against the C library.  llvm.c prefixed it
# with the module path like any toke function, so `m=ab; f=labs(x:i64):i64;`
# declared and called `@ab_labs`, which nothing defines: every user extern
# failed at ld ("Undefined symbols: _ab_labs").  prepass_funcs now leaves a
# body-less declaration unprefixed and mangle_fn_name does not prefix a
# registered extern at call sites.
#
# WHAT THIS DRIVES.  A module with a dotted path (the prefix is `ab_cd_`)
# declares two libc functions, labs and strlen, and calls one of them from a
# toke function with a body, so both call-site paths are used.  It must
# build, link against the real C library, and print 42 and 5.  The IR must
# declare @labs/@strlen bare and still define the bodied function prefixed.
#
# Negative control: drop the `decl_has_body &&` guard in prepass_funcs and
# the `!sig->is_internal` return in mangle_fn_name — the build fails at ld
# on _ab_cd_labs.
#
# Story: 127.212

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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c112_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C112: a user extern links to the C symbol it names"
echo "---------------------------------------------------"

cat > "${WORK}/ext.tk" <<'EOF2'
m=ab.cd;
i=j:std.json;
f=labs(x:i64):i64;
f=strlen(s:$str):i64;
f=twice(x:i64):i64{<labs(x)*2};
f=main():i64{j.print(twice(0-21));j.print(strlen("hello"));<0};
EOF2

if "${TKC}" "${WORK}/ext.tk" -o "${WORK}/ext" >"${WORK}/b.err" 2>&1; then
    pass "builds and links against libc"
    got="$("${WORK}/ext" | tr '\n' ' ' | sed 's/ $//')"
    if [ "${got}" = "42 5" ]; then pass "labs(-21)*2 and strlen(\"hello\") print 42 5"
    else fail "printed '${got}', want '42 5'"; fi
else
    fail "build failed: $(/usr/bin/grep -a -v W8001 "${WORK}/b.err" | head -c 400)"
fi

if "${TKC}" --emit-llvm "${WORK}/ext.tk" -o "${WORK}/ext.ll" >/dev/null 2>&1; then
    if /usr/bin/grep -a -q '^declare i64 @labs(i64)' "${WORK}/ext.ll" &&
       /usr/bin/grep -a -q '^declare i64 @strlen(' "${WORK}/ext.ll" &&
       ! /usr/bin/grep -a -q '@ab_cd_labs\|@ab_cd_strlen' "${WORK}/ext.ll"; then
        pass "IR declares @labs and @strlen unprefixed"
    else
        fail "IR externs: $(/usr/bin/grep -a -o '@[a-z_]*\(labs\|strlen\)' "${WORK}/ext.ll" | sort -u | tr '\n' ' ')"
    fi
    if /usr/bin/grep -a -q 'define dso_local fastcc i64 @ab_cd_twice(' "${WORK}/ext.ll"; then
        pass "a function with a body keeps the module prefix"
    else
        fail "bodied function lost its prefix"
    fi
else
    fail "--emit-llvm failed"
fi

# W8001 names the symbol that will actually be linked.
if /usr/bin/grep -a -q "extern function declaration 'labs'" "${WORK}/b.err"; then
    pass "W8001 names 'labs'"
else
    fail "W8001: $(/usr/bin/grep -a -o '"message":"extern[^"]*' "${WORK}/b.err" | head -1)"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
