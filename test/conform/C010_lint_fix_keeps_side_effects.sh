#!/usr/bin/env bash
# C010_lint_fix_keeps_side_effects.sh — `--lint --fix` must not delete an
# expression because nobody reads its result (story 127.82).
#
# THE DEFECT. `unused-let` fires on `let x = expr;` where x is never read,
# and its automatic fix deleted the whole statement — initialiser included.
# An unused *name* is not an unused *expression*, and the rule conflated
# them: `let ok=fs.mkdir(dir);` and `let st=proc.wait(pid);` had the
# directory creation and the process wait deleted along with the dead name.
# Applied to project scaffolding it removed the step that made the project.
#
# It compounded, too. Once the call was gone the module import it used
# became unreferenced, so the same fixpoint loop deleted the import on the
# next pass — one dead name silently taking a whole capability with it.
#
# THE FIX IS NOT "SWITCH THE RULE OFF". The diagnostic is right: the binding
# really is dead. What was wrong was the rewrite. So the fix is now offered
# only for an initialiser that is provably inert (an allowlist of node kinds
# in src/lint.c that cannot do anything but compute a value); anything with a
# call in it keeps the warning and loses the automatic fix. Case 4 below is
# what holds that line — it fails if the rule were repaired by silencing it.
#
# WHAT IS ASSERTED, AND WHY EACH CASE IS HERE:
#
#   1. NO FIX IS OFFERED for a binding whose initialiser calls something.
#      The warning must still be emitted — the binding IS dead.
#   2. --fix LEAVES THE SOURCE BYTE-IDENTICAL, and the import survives with
#      it. Asserted on bytes, not on "it still parses".
#   3. THE SIDE EFFECT STILL HAPPENS. The --fix'd program is compiled and
#      RUN, and the directory it was supposed to create is checked on disk.
#      This is the assertion that matters: a lint fix that type-checks and
#      builds proves nothing about whether the program still does its work.
#      A negative control runs the same program with the statement deleted
#      by hand and requires the directory to be ABSENT, so case 3 cannot
#      pass by the directory having been there all along.
#   4. AN INERT INITIALISER IS STILL AUTO-FIXED. `let unused=42;` must still
#      be detected, fixed, and gone from the source afterwards.
#
# Story: 127.82

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

MADE="${WORK}/scaffolded"

# ── Fixtures ─────────────────────────────────────────────────────────────
# `made` is never read. The call that binds it creates a directory.
cat > "${WORK}/effect.tk" <<EOF
m=test;
i=fs:std.file;
f=main():i64{
  let made=fs.mkdir("${MADE}");
  <0
};
EOF
cp "${WORK}/effect.tk" "${WORK}/effect_fixed.tk"

# The negative control: the same program with the statement removed, exactly
# as the old automatic fix removed it.
cat > "${WORK}/deleted.tk" <<EOF
m=test;
f=main():i64{
  <0
};
EOF

# An initialiser that cannot do anything.
cat > "${WORK}/inert.tk" <<'EOF'
m=test;
f=main():i64{
  let unused=42;
  <0
};
EOF
cp "${WORK}/inert.tk" "${WORK}/inert_fixed.tk"

# ── 1. warned, but no automatic fix ──────────────────────────────────────
effect_lint="$("${TKC}" --lint "${WORK}/effect.tk" 2>&1)"
expect "effect-still-warned" \
    "$(printf '%s' "${effect_lint}" | grep -c 'unused-let' || true)" "1"
expect "effect-fix-withdrawn" \
    "$(printf '%s' "${effect_lint}" | grep -c '"fix"' || true)" "0"

# ── 2. --fix leaves the source alone ─────────────────────────────────────
"${TKC}" --lint --fix "${WORK}/effect_fixed.tk" >/dev/null 2>&1
if cmp -s "${WORK}/effect.tk" "${WORK}/effect_fixed.tk"; then
    echo "PASS effect-source-untouched"
    PASS=$((PASS + 1))
else
    echo "FAIL effect-source-untouched"
    diff "${WORK}/effect.tk" "${WORK}/effect_fixed.tk" | sed 's/^/        /'
    FAIL=$((FAIL + 1))
fi
expect "effect-call-survives" \
    "$(grep -c 'fs\.mkdir' "${WORK}/effect_fixed.tk" || true)" "1"
expect "effect-import-survives" \
    "$(grep -c '^i=fs:std\.file;$' "${WORK}/effect_fixed.tk" || true)" "1"

# ── 3. the side effect still happens, end to end ─────────────────────────
rm -rf "${MADE}"
"${TKC}" "${WORK}/effect_fixed.tk" -o "${WORK}/effect.bin" >/dev/null 2>&1
expect "effect-compiles" "$?" "0"
# std.file writes are capability-gated; grant it so a CAP001 refusal cannot
# masquerade as a deleted side effect.
"${WORK}/effect.bin" --allow-write >/dev/null 2>&1
expect "effect-runs" "$?" "0"
if [ -d "${MADE}" ]; then
    echo "PASS side-effect-happened"
    PASS=$((PASS + 1))
else
    echo "FAIL side-effect-happened"
    echo "        ${MADE} was not created: the mkdir did not run"
    FAIL=$((FAIL + 1))
fi

# Negative control: without the statement the directory must NOT appear, so
# the assertion above is measuring the call and not the filesystem.
rm -rf "${MADE}"
"${TKC}" "${WORK}/deleted.tk" -o "${WORK}/deleted.bin" >/dev/null 2>&1
"${WORK}/deleted.bin" --allow-write >/dev/null 2>&1
if [ -d "${MADE}" ]; then
    echo "FAIL control-deletion-loses-effect"
    echo "        the directory appeared without the mkdir statement, so"
    echo "        side-effect-happened proves nothing"
    FAIL=$((FAIL + 1))
else
    echo "PASS control-deletion-loses-effect"
    PASS=$((PASS + 1))
fi

# ── 4. an inert initialiser is still auto-fixed ──────────────────────────
inert_lint="$("${TKC}" --lint "${WORK}/inert.tk" 2>&1)"
expect "inert-warned" \
    "$(printf '%s' "${inert_lint}" | grep -c 'unused-let' || true)" "1"
expect "inert-fix-offered" \
    "$(printf '%s' "${inert_lint}" | grep -c '"fix"' || true)" "1"
"${TKC}" --lint --fix "${WORK}/inert_fixed.tk" >/dev/null 2>&1
expect "inert-binding-removed" \
    "$(grep -c 'unused' "${WORK}/inert_fixed.tk" || true)" "0"
"${TKC}" "${WORK}/inert_fixed.tk" -o "${WORK}/inert.bin" >/dev/null 2>&1
expect "inert-fixed-still-compiles" "$?" "0"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
