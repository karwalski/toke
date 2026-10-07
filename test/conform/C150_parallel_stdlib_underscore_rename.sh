#!/usr/bin/env bash
# C150_parallel_stdlib_underscore_rename.sh — check_parallel_stdlib.py Rule 2
# sees the stale doc of a module renamed to drop an underscore (story 135.11a).
#
# THE DEFECT.  Rule 2 paired stdlib/<m>.md with docs/stdlib/<m>.md by EXACT
# basename.  113.2a renamed modules to drop underscores, so stdlib/llm_tool.md
# never matched docs/stdlib/llmtool.md and was classed as "no counterpart --
# the only copy, keep it": the orphan was protected, exactly backwards.
#
# WHAT THIS DRIVES: a throwaway copy of scripts/ + stdlib/ + docs/stdlib/ with
# an injected stdlib/llm_tool.md.  The gate must fail naming llmtool.md; the
# same tree without the injection must pass; and a verbatim underscore twin
# (json_stream.md) must still be recognised the old way.
#
# Negative control: revert the candidate loop to the exact-basename check ->
# case 1 passes rc 0 and this suite fails "injected orphan not caught".
#
# Story: 135.11a

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  ok   $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL $1"; }

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
mkdir -p "$T/scripts" "$T/docs"
cp "$REPO_ROOT/scripts/check_parallel_stdlib.py" \
   "$REPO_ROOT/scripts/check_parallel_stdlib_quarantine.txt" "$T/scripts/"
cp -R "$REPO_ROOT/stdlib" "$T/stdlib"
cp -R "$REPO_ROOT/docs/stdlib" "$T/docs/stdlib"
[ -f "$T/docs/stdlib/llmtool.md" ] || { echo "precondition: docs/stdlib/llmtool.md missing"; exit 1; }

echo "C150: parallel-stdlib Rule 2 and underscore renames (135.11a)"

# 1. Injected orphan under the pre-113.2a spelling must be caught.
echo "# stale" > "$T/stdlib/llm_tool.md"
out="$(python3 "$T/scripts/check_parallel_stdlib.py" 2>&1)"; rc=$?
if [ $rc -eq 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'stdlib/llm_tool.md .*docs/stdlib/llmtool.md'; then
  ok "injected stdlib/llm_tool.md fails naming docs/stdlib/llmtool.md"
else
  bad "injected orphan not caught (rc=$rc): $out"
fi
rm -f "$T/stdlib/llm_tool.md"

# 2. The same tree without the injection is clean.
out="$(python3 "$T/scripts/check_parallel_stdlib.py" 2>&1)"; rc=$?
[ $rc -eq 0 ] && ok "clean tree passes" || bad "clean tree failed (rc=$rc): $out"

# 3. A verbatim underscore twin still matches verbatim.  135.14 emptied the
#    quarantine and deleted stdlib/json_stream.md, so inject it, unlisted.
echo "# stale" > "$T/stdlib/json_stream.md"
out="$(python3 "$T/scripts/check_parallel_stdlib.py" 2>&1)"; rc=$?
if [ $rc -eq 1 ] && printf '%s' "$out" | /usr/bin/grep -q 'duplicates docs/stdlib/json_stream.md'; then
  ok "injected json_stream.md matched verbatim"
else
  bad "verbatim twin lost (rc=$rc): $out"
fi
rm -f "$T/stdlib/json_stream.md"

echo "C150: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
