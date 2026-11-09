#!/usr/bin/env bash
# C1110_claims_guards_scan_spec.sh -- both claims guards scan toke's spec/
# tree by default, and the published RFC there passes them (story 132.130).
#
# Until 132.130, DEFAULT_TARGETS in scripts/check_metrics_claims.py and
# scripts/check_canonical.py left out spec/, and `make check-claims-all`
# adds only the sibling repos plus the defaults (--self), so the published RFC
# (spec/rfc/draft-karwalski-toke-lang-00.md) was never read by any gate. Run
# directly it carried two rule-4 count findings: an "80-character" legacy
# profile (the fact sheet derives 86) and an undated "90/90 conformance tests".
#
# WHAT IS ASSERTED:
#   1. "spec" is in both guards' DEFAULT_TARGETS, and check_metrics_claims'
#      resolve_targets() with no arguments yields <repo>/spec.
#   2. check_metrics_claims.py and check_canonical.py exit 0 on spec/.
#   3. Negative control: a fixture stating the legacy profile is
#      "80-character" makes check_metrics_claims.py exit non-zero, so (2) is
#      not a guard that sees nothing.
#
# Measured: main 983cd5c9 (scripts/ spec/ docs/ snapshot) 1 passed, 4 failed --
# (1) fails for both guards and (2) fails with 2 findings (RFC :575,
# :1841); only the negative control passes. This branch: 5 passed.
#
# Story: 132.130
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
pass=0; fail=0
ok()  { echo "PASS: $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

for g in check_metrics_claims check_canonical; do
  if python3 -I -c "
import sys; sys.path.insert(0, sys.argv[1])
import importlib; m = importlib.import_module(sys.argv[2])
sys.exit(0 if 'spec' in m.DEFAULT_TARGETS else 1)" "${REPO_ROOT}/scripts" "$g"; then
    ok "$g DEFAULT_TARGETS includes spec"
  else
    bad "$g DEFAULT_TARGETS omits spec"
  fi
done

if python3 -I -c "
import os, sys; sys.path.insert(0, sys.argv[1])
import check_metrics_claims as m
sys.exit(0 if os.path.join(m.ROOT, 'spec') in m.resolve_targets([]) else 1)" \
    "${REPO_ROOT}/scripts"; then
  ok "resolve_targets() default includes <repo>/spec"
else
  bad "resolve_targets() default omits <repo>/spec"
fi

out="$(cd "${REPO_ROOT}" && python3 scripts/check_metrics_claims.py spec 2>&1)"; rc=$?
out2="$(cd "${REPO_ROOT}" && python3 scripts/check_canonical.py spec 2>&1)"; rc2=$?
if [ $rc -eq 0 ] && [ $rc2 -eq 0 ]; then
  ok "both guards pass on spec/"
else
  bad "a guard fails on spec/ (metrics rc=$rc, canonical rc=$rc2)"
  echo "$out" | grep -E '^  spec/' ; echo "$out2" | grep -E 'spec/' | head -5
fi

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/spec/rfc"
cat > "$tmp/spec/rfc/fixture.md" <<'EOF'
# Fixture

   During early development, toke used an 80-character legacy
   profile to maintain compatibility with existing LLM tokenizers.
EOF
(cd "${REPO_ROOT}" && python3 scripts/check_metrics_claims.py "$tmp/spec" >/dev/null 2>&1); nrc=$?
if [ $nrc -ne 0 ]; then
  ok "negative control: '80-character' legacy profile is rejected"
else
  bad "negative control: '80-character' legacy profile passed the guard"
fi

echo "C1110: ${pass} passed, ${fail} failed"
[ $fail -eq 0 ]
