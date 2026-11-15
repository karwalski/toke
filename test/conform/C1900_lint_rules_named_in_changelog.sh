#!/usr/bin/env bash
# C1900_lint_rules_named_in_changelog.sh -- every lint rule id in src/lint.c
# is named in CHANGELOG.md and in docs/compiler/lint-rules-v1.md, and
# `make check-lint-rules` (in `make ci`) says so (story 138.41).
#
# 3.2.0 added the nested-if-ladder rule and its changelog named no rule id;
# loke's lint ratchet went 552 to 737 on unchanged code (loke handoff
# 2026-11-15, N-7). scripts/check_lint_rules_named.py now fails when a rule
# id is not named in both places, and with --since REF when an id added or
# removed since REF is not named in the changelog's topmost section.
#
# WHAT IS ASSERTED:
#   1. the script's --selftest passes;
#   2. it passes on this tree and lists nested-if-ladder among the ids;
#   3. `ci:` in the Makefile runs check-lint-rules;
#   negative controls, each on a copy with the defect verified in place:
#   4. CHANGELOG.md without nested-if-ladder fails, naming that id;
#   5. src/lint.c with a new rule id fails twice (changelog and reference);
#   6. the reference without nested-if-ladder's table row fails;
#   7. --since in a scratch git repository: a rule added after the tag and
#      named only in an older section fails; named under ## Unreleased it
#      passes; an unreadable REF exits 2.
#
# Measured: on main 987d9f6f (CHANGELOG.md before 138.41's addendum) the
# script reports 13 findings, one per rule id; this branch passes.
#
# Story: 138.41
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CHK="${REPO_ROOT}/scripts/check_lint_rules_named.py"
pass=0; fail=0
ok()  { echo "PASS: $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1900_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

# 1. selftest
if python3 -I "$CHK" --selftest >"$WORK/st.txt" 2>&1; then
  ok "selftest: $(tail -1 "$WORK/st.txt")"
else
  bad "selftest failed"; cat "$WORK/st.txt"
fi

# 2. this tree
out="$(python3 -I "$CHK" 2>&1)"; rc=$?
if [ $rc -eq 0 ] && python3 -I "$CHK" --list | grep -qx 'nested-if-ladder'; then
  ok "tree passes and lists nested-if-ladder"
else
  bad "tree: rc=$rc"; echo "$out" | tail -5
fi

# 3. wired into make ci
if grep -qE '^ci:.*[[:space:]]check-lint-rules([[:space:]]|$)' "${REPO_ROOT}/Makefile"; then
  ok "make ci runs check-lint-rules"
else
  bad "check-lint-rules is not a prerequisite of ci: in the Makefile"
fi

# 4. changelog without nested-if-ladder
grep -v 'nested-if-ladder' "${REPO_ROOT}/CHANGELOG.md" >"$WORK/CHANGELOG.md"
if grep -q 'nested-if-ladder' "$WORK/CHANGELOG.md"; then
  bad "control 4 did not apply (nested-if-ladder still in the copy)"
else
  out="$(python3 -I "$CHK" --changelog "$WORK/CHANGELOG.md" 2>&1)"; rc=$?
  if [ $rc -eq 1 ] && echo "$out" | grep -q '`nested-if-ladder`: not named in CHANGELOG.md'; then
    ok "a rule missing from the changelog fails, by id"
  else
    bad "changelog control: rc=$rc"; echo "$out" | tail -3
  fi
fi

# 5. a new rule id in lint.c
cp "${REPO_ROOT}/src/lint.c" "$WORK/lint.c"
printf '\nstatic int c1900(LintResult *out){ return lint_push(out, "zz-c1900-rule", "m", 0, 0, 0, LINT_HINT, 0, 0, 0, NULL); }\n' >>"$WORK/lint.c"
if ! grep -q '"zz-c1900-rule"' "$WORK/lint.c"; then
  bad "control 5 did not apply"
else
  out="$(python3 -I "$CHK" --lint-src "$WORK/lint.c" 2>&1)"; rc=$?
  n=$(echo "$out" | grep -c '`zz-c1900-rule`')
  if [ $rc -eq 1 ] && [ "$n" -eq 2 ]; then
    ok "a new rule id named nowhere fails twice (changelog, reference)"
  else
    bad "new-rule control: rc=$rc, findings naming it=$n"; echo "$out" | tail -3
  fi
fi

# 6. reference without nested-if-ladder's row
grep -v '^| `nested-if-ladder` |' "${REPO_ROOT}/docs/compiler/lint-rules-v1.md" >"$WORK/ref.md"
if grep -q '^| `nested-if-ladder` |' "$WORK/ref.md"; then
  bad "control 6 did not apply"
else
  out="$(python3 -I "$CHK" --reference "$WORK/ref.md" 2>&1)"; rc=$?
  if [ $rc -eq 1 ] && echo "$out" | grep -q '`nested-if-ladder`: no row'; then
    ok "a rule missing from the reference table fails, by id"
  else
    bad "reference control: rc=$rc"; echo "$out" | tail -3
  fi
fi

# 7. --since, in a scratch repository (no dependence on this clone's tags)
R="$WORK/repo"
mkdir -p "$R/scripts" "$R/src" "$R/docs/compiler"
cp "$CHK" "$R/scripts/"
printf 'int f(void){ return lint_push(out, "old-rule", "m"); }\n' >"$R/src/lint.c"
printf '# Changelog\n\n## Unreleased\n\n## 1.0\n\n`old-rule`\n' >"$R/CHANGELOG.md"
printf '| Rule ID |\n|---|\n| `old-rule` | hint |\n| `new-rule` | hint |\n' >"$R/docs/compiler/lint-rules-v1.md"
G="git -C $R -c user.name=c1900 -c user.email=c1900@example.invalid -c commit.gpgsign=false -c tag.gpgsign=false"
if $G init -q && $G add -A && $G commit -qm old && $G tag vold; then
  printf 'int g(void){ return lint_push(out, "new-rule", "m"); }\n' >>"$R/src/lint.c"
  printf '# Changelog\n\n## Unreleased\n\n## 1.0\n\n`old-rule` `new-rule`\n' >"$R/CHANGELOG.md"
  out="$(python3 -I "$R/scripts/check_lint_rules_named.py" --since vold 2>&1)"; rc=$?
  if [ $rc -eq 1 ] && echo "$out" | grep -q '`new-rule`: added since vold'; then
    ok "--since: an added rule named only in an older section fails"
  else
    bad "--since negative: rc=$rc"; echo "$out" | tail -3
  fi
  printf '# Changelog\n\n## Unreleased\n\n`new-rule` (new)\n\n## 1.0\n\n`old-rule`\n' >"$R/CHANGELOG.md"
  out="$(python3 -I "$R/scripts/check_lint_rules_named.py" --since vold 2>&1)"; rc=$?
  if [ $rc -eq 0 ]; then
    ok "--since: an added rule named under ## Unreleased passes"
  else
    bad "--since positive: rc=$rc"; echo "$out" | tail -3
  fi
  python3 -I "$R/scripts/check_lint_rules_named.py" --since nosuchref >/dev/null 2>&1; rc=$?
  if [ $rc -eq 2 ]; then ok "--since: an unreadable REF exits 2"; else bad "--since bad ref: rc=$rc"; fi
else
  bad "could not set up the scratch git repository"
fi

echo "C1900: ${pass} passed, ${fail} failed"
[ "$fail" -eq 0 ]
