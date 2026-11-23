#!/usr/bin/env bash
# C2151_migrate_str_index_byte_compare.sh -- `tkc --migrate` rewrites a legacy
# str index that is compared with another str index, or with a one-byte str
# literal, to `.charat(i)` (no union), so the migrated comparison compiles
# (story 127.512).
#
# THE DEFECT (integrate/wave-a-1122 05010ab8).  A str index in a str context
# migrated to `.slice(i;i+1)`, which is `str!$sliceerr` since 127.462, so
# `a[0]=b[1]` and `v[i]="a"` migrated to comparisons that do not compile
# (E4031).  test/migrate/str_index pinned two of those E4031s.
#
# THE FIX (owner answer R2: single-character reads go through the byte read,
# no union; no mechanical filler arm).  When both sides of `=`/`==`/`!=` are
# one-character reads, the comparison is a byte comparison: each str index
# becomes `.charat(i)` and a one-byte literal becomes its byte code (`"a"` ->
# 97, `"\n"` -> 10, `"\""` -> 34).  A str index used as a str (returned,
# concatenated, printed, compared with a longer string) still becomes
# `.slice(i;i+1)` with the 127.510 note: std.str has no non-union
# one-character str read, so the author gives it an error arm.
#
# WHAT IS ASSERTED
#   1. index = index, index = "a", "\n" = index, index = "\"" all migrate to
#      `.charat` with literal codes, and stderr reports no `.slice`.
#   2. That output compiles and runs, printing the four matches and not the
#      mismatch.
#   3. index = "ab" (two bytes) still migrates to `.slice(0;1)`, with the note.
#
# Negative control (TKC=<other tkc>): on v3.2.0 assertions 1 and 3 fail: every
# comparison migrates to `.slice(i;i+1)` (there a plain str, so 2 passes).
#
# Story: 127.512
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
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2151_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C2151: --migrate str index compared one character to one -> .charat"
echo "-------------------------------------------------------------------"

cat > "${WORK}/c.legacy.tk" <<'EOF'
M=x;
I=s:std.str;
F=same(a:Str;b:Str):bool{<a[0]=b[1]};
F=isa(v:Str;i:i64):bool{<v[i]="a"};
F=isnl(v:Str):bool{<"\n"=v[0]};
F=quote(v:Str):bool{<v[0]="\""};
F=main():i64{
  if(same("xa";"ax")){s.print("same;")};
  if(same("ab";"xy")){s.print("BAD;")};
  if(isa("ba";1)){s.print("isa;")};
  if(isnl("\nz")){s.print("nl;")};
  if(quote("\"q")){s.print("quote;")};
  <0
};
EOF
cat > "${WORK}/t.legacy.tk" <<'EOF'
M=x;
F=two(v:Str):bool{<v[0]="ab"};
F=main():i64{<0};
EOF

"${TKC}" --migrate "${WORK}/c.legacy.tk" >"${WORK}/c.tk" 2>"${WORK}/c.err"; rc=$?
ok=1
for want in 'f=same(a:str;b:str):bool{<a.charat(0)==b.charat(1)};' \
            'f=isa(v:str;i:i64):bool{<v.charat(i)==97};' \
            'f=isnl(v:str):bool{<10==v.charat(0)};' \
            'f=quote(v:str):bool{<v.charat(0)==34};'; do
    grep -qF "${want}" "${WORK}/c.tk" || ok=0
done
if [ "${rc}" -eq 0 ] && [ "${ok}" -eq 1 ] && ! grep -q 'slice' "${WORK}/c.tk" \
   && grep -q '0 str index(es) → .slice(i;i+1), 5 → .charat(i)' "${WORK}/c.err" \
   && ! grep -q 'sliceerr' "${WORK}/c.err"; then
    pass "one-character comparisons migrate to .charat with literal byte codes, no slice note"
else
    fail "rc=${rc}, output: $(cat "${WORK}/c.tk") stderr: $(cat "${WORK}/c.err")"
fi

if "${TKC}" "${WORK}/c.tk" -o "${WORK}/c.bin" >"${WORK}/c.build" 2>&1; then
    out="$("${WORK}/c.bin" 2>&1)"
    if [ "${out}" = "same;isa;nl;quote;" ]; then
        pass "the migrated output compiles and compares bytes as the legacy source did"
    else
        fail "migrated program printed: ${out}"
    fi
else
    fail "migrated output does not compile: $(head -c 300 "${WORK}/c.build")"
fi

"${TKC}" --migrate "${WORK}/t.legacy.tk" >"${WORK}/t.tk" 2>"${WORK}/t.err"; rc=$?
if [ "${rc}" -eq 0 ] && grep -qF '<v.slice(0;1)=="ab"' "${WORK}/t.tk" \
   && grep -q "note: each .slice(i;i+1) is str!\$sliceerr" "${WORK}/t.err"; then
    pass "a comparison with a two-byte literal keeps .slice and the union note"
else
    fail "rc=${rc}, output: $(cat "${WORK}/t.tk") stderr: $(cat "${WORK}/t.err")"
fi

echo ""
echo "C2151: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
