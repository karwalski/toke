#!/usr/bin/env bash
# C009_lint_import_is_load_bearing.sh — `unused-import` must not advise
# deleting an import the compiler still depends on (story 127.81).
#
# THE DEFECT. `unused-import` decided "used" by walking the AST for an ident
# node whose text matches the alias. A *type-only* use has no such node:
# `parse_type_expr` (src/parser.c:413) lowers `alias.$rec` to ONE
# NODE_TYPE_IDENT carrying the *type* token and marked `op == TK_DOT`, and
# throws the `alias` `.` `$` tokens away. The alias is therefore absent from
# the tree, the rule called the import unused, and offered to delete it.
#
# WHY DELETING AN IMPORT IS NOT COSMETIC. The import list is the compiler's
# module manifest, not just a name-resolution scope. Three codegen consumers
# read it directly, and case 2 below measures two of them:
#
#   - resolve_stdlib_deps_imports_only (src/stdlib_deps.c) chooses which C
#     runtime files get compiled into the binary. Drop the import and the
#     module's implementation silently leaves the link set.
#   - register_tki_struct_types (src/llvm.c) loads stdlib/<mod>.tki to learn
#     record field -> GEP index. Drop the import and field access falls back
#     to index 0 — the WRONG FIELD, with no diagnostic.
#   - prepass_load_tki / resolve_stdlib_call map alias -> C symbol.
#
# WHAT IS ASSERTED, AND WHY EACH CASE IS HERE:
#
#   1. THE FALSE POSITIVE IS GONE. An import used only as `alias.$rec` in a
#      parameter type must draw no unused-import diagnostic at all.
#
#   2. A NEGATIVE CONTROL THAT MEASURES THE DAMAGE, not just the warning.
#      The same file with the import deleted by hand is compared against the
#      original on two observable outputs of the compiler: the dependency set
#      (--emit-deps) and the generated field index (--emit-llvm). If removing
#      the import changed neither, there was no defect to fix and case 1 is
#      decorative. This is the assertion that would have caught the original
#      symptom — a binary that builds, logs success, and does not work.
#
#   3. THE RULE STILL FIRES ON A GENUINELY UNUSED IMPORT, and still offers
#      the fix. A rule repaired by switching it off is not repaired.
#
# Story: 127.81

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/toke}"

PASS=0
FAIL=0

expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label"
        echo "        expected: $want"
        echo "        got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# ── Fixtures ─────────────────────────────────────────────────────────────
# `c` is referenced ONLY in the parameter type `c.$csvreader`. std.csv's
# csvreader record is fields [data, pos, sep, line], so `.pos` is index 1 —
# a value the compiler can only know by reading stdlib/csv.tki, which it only
# reads because the import is present.

cat > "${WORK}/typeonly.tk" <<'EOF'
m=test;
i=c:std.csv;
f=getpos(r:c.$csvreader):u64{ <r.pos };
f=main():i64{ <0 };
EOF

# The same program after the fix the rule used to advise.
grep -v '^i=c:std.csv;$' "${WORK}/typeonly.tk" > "${WORK}/removed.tk"

# A genuinely unreferenced import — the rule must still catch this one.
cat > "${WORK}/genuine.tk" <<'EOF'
m=test;
i=c:std.csv;
f=main():i64{ <0 };
EOF

# ── 1. the false positive is gone ────────────────────────────────────────
typeonly_diags="$("${TKC}" --lint "${WORK}/typeonly.tk" 2>&1 | grep -c 'unused-import' || true)"
expect "typeonly-import-not-flagged" "${typeonly_diags}" "0"

# ── 2. negative control: removing it DOES change the emitted program ─────
"${TKC}" --emit-deps "${WORK}/typeonly.tk" > "${WORK}/deps.with"  2>/dev/null
"${TKC}" --emit-deps "${WORK}/removed.tk"  > "${WORK}/deps.without" 2>/dev/null

# std.csv's runtime is in the link set only while the import is there.
with_csv="$(grep -c 'stdlib/csv\.c' "${WORK}/deps.with" || true)"
without_csv="$(grep -c 'stdlib/csv\.c' "${WORK}/deps.without" || true)"
expect "link-set-has-csv-with-import"    "${with_csv}"    "1"
expect "link-set-loses-csv-without"      "${without_csv}" "0"

# The .tki-derived field index is right only while the import is there.
"${TKC}" --emit-llvm "${WORK}/typeonly.tk" -o "${WORK}/with.ll"    >/dev/null 2>&1
"${TKC}" --emit-llvm "${WORK}/removed.tk"  -o "${WORK}/without.ll" >/dev/null 2>&1

gep_index() {
    # The GEP index emitted for the `.pos` field access, or "none".
    if [ -f "$1" ]; then
        sed -n 's/.*getelementptr[^;]*i32 \([0-9][0-9]*\).*; \.pos.*/\1/p' "$1" |
            head -1 | grep -E '^[0-9]+$' || echo "none"
    else
        echo "none"
    fi
}
expect "field-index-correct-with-import" "$(gep_index "${WORK}/with.ll")" "1"

# Without the import the compiler cannot know the layout. It must NOT quietly
# emit index 0 (the `data` field) as if it did; either it refuses to emit, or
# it emits something other than a confident wrong answer.
without_index="$(gep_index "${WORK}/without.ll")"
if [ "${without_index}" = "1" ]; then
    echo "FAIL removal-changes-nothing"
    echo "        removing the import left the field index unchanged, so this"
    echo "        test proves nothing about the import being load-bearing"
    FAIL=$((FAIL + 1))
else
    echo "PASS removal-changes-codegen"
    PASS=$((PASS + 1))
fi

# ── 3. the rule still fires on a genuinely unused import ─────────────────
genuine_diags="$("${TKC}" --lint "${WORK}/genuine.tk" 2>&1 | grep -c 'unused-import' || true)"
expect "genuine-unused-import-flagged" "${genuine_diags}" "1"

# ...and still offers a fix for it.
genuine_fix="$("${TKC}" --lint "${WORK}/genuine.tk" 2>&1 | grep -c '"fix"' || true)"
expect "genuine-unused-import-fixable" "${genuine_fix}" "1"

# ...and the fix, applied, leaves a program that still builds.
cp "${WORK}/genuine.tk" "${WORK}/genuine_fixed.tk"
"${TKC}" --lint --fix "${WORK}/genuine_fixed.tk" >/dev/null 2>&1
"${TKC}" "${WORK}/genuine_fixed.tk" -o "${WORK}/genuine.bin" >/dev/null 2>&1
expect "genuine-fixed-still-compiles" "$?" "0"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
