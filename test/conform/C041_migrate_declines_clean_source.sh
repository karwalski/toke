#!/usr/bin/env bash
# C041_migrate_declines_clean_source.sh — `tkc --migrate` must not damage
# source it has nothing to do to, and must not damage the source it does
# (story 127.57).
#
# THE DEFECT. `--migrate` is a text rewriter with no notion of whether its
# input needs rewriting. It ran its whole prepass → token → six-postpass chain
# over anything handed to it, including source that was already valid v0.4.
# Measured over this tree on 2026-09-24, tkc 2.8.0, before the fix:
#
#     files that --check clean          : 497
#       still check clean after migrate :  461
#       NO LONGER check clean           :   36   <-- incl. stdlib/encoding.tk
#                                                     and stdlib/sha256.tk
#     files containing block comments   : 124
#       losing at least one comment     : 124   (216 of 216 blocks, 100%)
#     --migrate exited non-zero         :   0   <-- in every one of those cases
#
# Among the comment casualties was test/conform/fixtures/roundtrip/comments.tk,
# the round-trip fixture named for the thing being destroyed.
#
# THE DECISION (127.57). Repair, or refuse? Both, at different layers:
#
#   * Source that compiles under the current profile is BY DEFINITION already
#     in the syntax --migrate exists to produce. There is nothing for the
#     migrator to do to it and everything for it to lose, so it now DECLINES.
#     That removes the entire 36-file and 124-file populations at a stroke,
#     including every failure mode nobody has diagnosed yet.
#   * Declining must not destroy anything either. --migrate writes to stdout
#     and is used through a redirect, so a refusal that printed nothing would
#     truncate the user's file — worse than the corruption it replaces. The
#     source is therefore echoed BYTE FOR BYTE and the exit code carries the
#     signal instead.
#   * Refusing on clean source does nothing for source that genuinely needs
#     migrating, so two corruptions on that path are fixed outright: the
#     doubled sigil (`@($E)` → `@($$e)`, unparseable) and the `t=` type
#     position false positive (`t=u+1;` → `t=$u+1;`, uncompilable).
#
# WHAT IS ASSERTED, AND WHY EACH CASE IS HERE:
#
#   1. CLEAN SOURCE IS DECLINED, AND ECHOED VERBATIM. Asserted on bytes,
#      including the comments, and on a non-zero exit code. A refusal nobody
#      can detect is the defect again in a different costume.
#   2. THE DECLINE IS NOT A BLANKET REFUSAL. Source that does NOT compile is
#      still migrated, and the output still compiles. Without this case, case
#      1 would pass on a --migrate that had simply been switched off.
#   3. THE TWO CORRUPTIONS ARE GONE ON GENUINE INPUT. `@($E)` and a `t`
#      reassignment inside a function body, in files that do not compile as
#      they stand, migrate to source that does.
#   4. TREE-WIDE MEASUREMENT. Over every .tk in the repository that compiles,
#      --migrate must lose nothing: no file stops compiling, no comment block
#      disappears. This is the shape of the original measurement, re-run as
#      an assertion.
#   5. A NEGATIVE CONTROL. The clean-source path is forced through the old
#      behaviour (TKC_MIGRATE_FORCE=1) and case 4's measurement re-run; it
#      must go red. A gate whose corpus is 497 green files and which has never
#      been watched to fail is not evidence.
#
# Story: 127.57

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

# Already valid v0.4, with comments, and with the two shapes that used to be
# corrupted (`t` reassigned in a body; a sigilled user type in an array type).
cat > "${WORK}/clean.tk" <<'EOF'
(* header comment *)
m=clean;
t=$pair{a:i64;b:i64};
f=main():i64{
  (* body comment *)
  let t=mut.1;
  let u=2;
  t=u+1;      (* t is a variable here, not a type declaration *)
  let xs=@($pair);
  <t+xs.len()
};
EOF

# Genuinely pre-v0.4: uppercase declaration keywords and an uppercase type
# name. Does not compile as it stands, so --migrate must run on it.
cat > "${WORK}/legacy.tk" <<'EOF'
M=legacy;
T=$Pair{a:i64;b:i64};
F=main():i64{
  let xs=@($Pair);
  <xs.len()
};
EOF

# Genuinely pre-v0.4 and containing a `t` reassignment in a function body.
cat > "${WORK}/legacy_t.tk" <<'EOF'
M=legt;
F=main():i64{
  let t=mut.1;
  let u=2;
  t=u+1;
  <t
};
EOF

# ── 1. clean source is declined, and echoed verbatim ─────────────────────

"${TKC}" --check "${WORK}/clean.tk" >/dev/null 2>&1
expect "1-fixture-really-is-clean" "$?" "0"

"${TKC}" --migrate "${WORK}/clean.tk" > "${WORK}/clean.out" 2>"${WORK}/clean.err"
clean_rc=$?
expect "1a-clean-source-declined" \
    "$([ "${clean_rc}" -ne 0 ] && echo nonzero || echo zero)" "nonzero"
if cmp -s "${WORK}/clean.tk" "${WORK}/clean.out"; then
    echo "PASS 1b-echoed-byte-for-byte"
    PASS=$((PASS + 1))
else
    echo "FAIL 1b-echoed-byte-for-byte"
    diff "${WORK}/clean.tk" "${WORK}/clean.out" | sed 's/^/        /'
    FAIL=$((FAIL + 1))
fi
# The comments are the specific thing that used to go, so name them.
expect "1c-comments-survive" \
    "$(grep -c '(\*' "${WORK}/clean.out" || true)" \
    "$(grep -c '(\*' "${WORK}/clean.tk" || true)"
expect "1d-refusal-is-explained-on-stderr" \
    "$(grep -c 'nothing to migrate' "${WORK}/clean.err" || true)" "1"

# ── 2. the decline is not a blanket refusal ──────────────────────────────

"${TKC}" --check "${WORK}/legacy.tk" >/dev/null 2>&1
expect "2-fixture-really-does-not-compile" \
    "$([ "$?" -ne 0 ] && echo dirty || echo clean)" "dirty"

"${TKC}" --migrate "${WORK}/legacy.tk" > "${WORK}/legacy.out" 2>/dev/null
expect "2a-legacy-migrate-exits-zero" "$?" "0"
if cmp -s "${WORK}/legacy.tk" "${WORK}/legacy.out"; then
    echo "FAIL 2b-legacy-was-actually-migrated"
    echo "        output is identical to the input: --migrate did nothing,"
    echo "        so case 1 is measuring a tool that has been switched off"
    FAIL=$((FAIL + 1))
else
    echo "PASS 2b-legacy-was-actually-migrated"
    PASS=$((PASS + 1))
fi
"${TKC}" --check "${WORK}/legacy.out" >/dev/null 2>&1
expect "2c-migrated-legacy-compiles" "$?" "0"

# ── 3. the two corruptions are gone on genuine input ─────────────────────

# `@($Pair)` used to become `@($$pair)`: TK_DOLLAR wrote its '$' and the
# TK_TYPE_IDENT branch unconditionally wrote another.
expect "3a-no-doubled-sigil" \
    "$(grep -c '\$\$' "${WORK}/legacy.out" || true)" "0"
expect "3a-sigil-present-once" \
    "$(grep -c '@(\$pair)' "${WORK}/legacy.out" || true)" "1"

"${TKC}" --migrate "${WORK}/legacy_t.tk" > "${WORK}/legacy_t.out" 2>/dev/null
expect "3b-legacy-t-migrate-exits-zero" "$?" "0"
# `t=u+1;` used to become `t=$u+1;` — the "after t=" type-position heuristic
# firing on a reassignment inside a function body.
expect "3b-no-spurious-sigil-on-rhs" \
    "$(grep -c 't=\$u' "${WORK}/legacy_t.out" || true)" "0"
"${TKC}" --check "${WORK}/legacy_t.out" >/dev/null 2>&1
expect "3b-migrated-t-compiles" "$?" "0"

# ── 4. tree-wide: --migrate loses nothing on source that compiles ────────

count_comment_blocks() {
    awk 'BEGIN{c=0} {c+=gsub(/\(\*/,"")} END{print c+0}' "$1"
}

measure() {
    # measure <list-file> [force] → "<clean> <broken> <comment_files_lost>"
    local list="$1" force="${2:-}" clean=0 broke=0 clost=0 f n m
    while IFS= read -r f; do
        "${TKC}" --check "$f" >/dev/null 2>&1 || continue
        clean=$((clean + 1))
        if [ -n "${force}" ]; then
            TKC_MIGRATE_FORCE=1 "${TKC}" --migrate "$f" > "${WORK}/m.tk" 2>/dev/null
        else
            "${TKC}" --migrate "$f" > "${WORK}/m.tk" 2>/dev/null
        fi
        "${TKC}" --check "${WORK}/m.tk" >/dev/null 2>&1 || {
            broke=$((broke + 1))
            [ "${broke}" -le 3 ] && echo "        no longer compiles: $f" >&2
        }
        n="$(count_comment_blocks "$f")"
        [ "$n" -eq 0 ] && continue
        m="$(count_comment_blocks "${WORK}/m.tk")"
        [ "$m" -lt "$n" ] && clost=$((clost + 1))
    done < "$list"
    echo "${clean} ${broke} ${clost}"
}

find "${REPO_ROOT}" -name '*.tk' -type f ! -path '*/.git/*' | sort > "${WORK}/all.txt"
read -r T_CLEAN T_BROKE T_CLOST <<< "$(measure "${WORK}/all.txt")"

expect "4-corpus-is-not-empty" \
    "$([ "${T_CLEAN}" -ge 100 ] && echo enough || echo "only ${T_CLEAN}")" "enough"
expect "4a-no-clean-file-stops-compiling" "${T_BROKE}" "0"
expect "4b-no-file-loses-a-comment-block" "${T_CLOST}" "0"
echo "      (swept ${T_CLEAN} files that --check clean)"

# ── 5. negative control ──────────────────────────────────────────────────
#
# TKC_MIGRATE_FORCE=1 puts the clean-source path back the way it was. Case 4's
# measurement must then go red — otherwise case 4 is 497 files proving nothing.
#
# The control runs over a named handful rather than the whole tree: these are
# two of the files the 127.57 measurement singled out — a genuine stdlib source
# that stopped compiling, and the round-trip fixture named for comments, which
# lost all of its. Re-sweeping 497 files to learn the same thing would double
# this suite's runtime for no extra information.

: > "${WORK}/control.txt"
for rel in stdlib/encoding.tk stdlib/sha256.tk \
           test/conform/fixtures/roundtrip/comments.tk; do
    [ -f "${REPO_ROOT}/${rel}" ] && echo "${REPO_ROOT}/${rel}" >> "${WORK}/control.txt"
done
expect "5-control-corpus-present" \
    "$(wc -l < "${WORK}/control.txt" | tr -d ' ')" "3"

TKC_MIGRATE_FORCE=1 "${TKC}" --migrate "${WORK}/clean.tk" > "${WORK}/forced.out" 2>/dev/null
if cmp -s "${WORK}/clean.tk" "${WORK}/forced.out"; then
    echo "FAIL 5a-injection-landed"
    echo "        TKC_MIGRATE_FORCE=1 changed nothing, so the control below"
    echo "        is not exercising the old path and case 4 stands unwitnessed"
    FAIL=$((FAIL + 1))
else
    echo "PASS 5a-injection-landed"
    PASS=$((PASS + 1))
fi

read -r C_CLEAN C_BROKE C_CLOST <<< "$(measure "${WORK}/control.txt" force 2>/dev/null)"
expect "5b-control-breaks-compilation" \
    "$([ "${C_BROKE}" -gt 0 ] && echo red || echo green)" "red"
expect "5c-control-loses-comments" \
    "$([ "${C_CLOST}" -gt 0 ] && echo red || echo green)" "red"
echo "      (control: ${C_BROKE} of ${C_CLEAN} stopped compiling, ${C_CLOST} lost comments)"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
