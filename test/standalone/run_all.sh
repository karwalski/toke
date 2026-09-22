#!/bin/bash
# run_all.sh — compile and run all standalone toke test files.
# Each test_*.tk is compiled via the toke compiler, linked with its
# dependencies, and executed.  Output lines starting with PASS/FAIL
# are tallied.

# 127.53: these default to the main checkout, as they always have, but are
# now overridable.  They were hard absolute paths, so running this script from
# a git worktree silently measured the MAIN checkout's compiler and reported a
# pass for a binary the branch had never built -- a gate that proves nothing.
# Unset, the behaviour below is byte-identical to before.
ROOT=${TOKE_ROOT:-/Users/matthew.watt/tk/toke}
TOKE=${TOKE:-$ROOT/toke}
STDLIB=${STDLIB:-$ROOT/src/stdlib}
VENDOR=${VENDOR:-$ROOT/stdlib/vendor}
DIR=$(dirname "$0")
TOTAL_PASS=0; TOTAL_FAIL=0

for tk in "$DIR"/test_*.tk; do
  # *.blocked.tk: a test whose fix still needs a src/llvm.c change held by
  # another story. Kept next to the live tests so it is picked up by renaming
  # once that change lands; never auto-run.
  case "$tk" in *.blocked.tk) continue;; esac
  name=$(basename "$tk" .tk)
  echo "=== $name ==="

  # Compile to LLVM IR
  "$TOKE" --allow-all --emit-llvm --out "/tmp/${name}.ll" "$tk" 2>/dev/null
  if [ $? -ne 0 ]; then echo "  COMPILE ERROR"; TOTAL_FAIL=$((TOTAL_FAIL+1)); continue; fi

  # Gather C dependencies (--emit-deps includes vendor sources when needed)
  DEPS=$("$TOKE" --emit-deps "$tk" 2>/dev/null | grep -v '^---' | sort -u)

  # Build clang args: each dep gets its own -x c <file>
  DEP_ARGS=""
  for dep in $DEPS; do
    DEP_ARGS="$DEP_ARGS -x c $dep"
  done

  clang -std=c99 -D_GNU_SOURCE -O1 -iquote "$STDLIB" \
    -I "$VENDOR/cmark/src" -I "$VENDOR/tomlc99" \
    -x ir "/tmp/${name}.ll" \
    $DEP_ARGS \
    -o "/tmp/${name}_bin" -lm -lz 2>/dev/null

  if [ $? -ne 0 ]; then echo "  LINK ERROR"; TOTAL_FAIL=$((TOTAL_FAIL+1)); continue; fi

  # Run and count pass/fail
  output=$("/tmp/${name}_bin" 2>&1)
  pass=$(echo "$output" | grep -c "^PASS")
  fail=$(echo "$output" | grep -c "^FAIL")
  echo "$output" | grep "FAIL"
  echo "  $pass passed, $fail failed"
  TOTAL_PASS=$((TOTAL_PASS+pass))
  TOTAL_FAIL=$((TOTAL_FAIL+fail))

  rm -f "/tmp/${name}.ll" "/tmp/${name}_bin"
done

echo ""
echo "=== TOTAL: $TOTAL_PASS passed, $TOTAL_FAIL failed ==="
exit $TOTAL_FAIL
