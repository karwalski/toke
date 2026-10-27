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
# 131.45: the default is now the checkout this script lives in, not the main
# checkout, so `make test-standalone` in a worktree tests that worktree.
ROOT=${TOKE_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
TOKE=${TOKE:-$ROOT/toke}
STDLIB=${STDLIB:-$ROOT/src/stdlib}
VENDOR=${VENDOR:-$ROOT/stdlib/vendor}
DIR=$(dirname "$0")
# 131.45: per-run scratch dir.  Fixed /tmp/<name>.ll paths collided when two
# checkouts ran the suite at once (one run linked the other's IR).
WORK=$(mktemp -d "${TMPDIR:-/tmp}/tk_standalone.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT
TOTAL_PASS=0; TOTAL_FAIL=0

for tk in "$DIR"/test_*.tk; do
  # *.blocked.tk: a test whose fix still needs a src/llvm.c change held by
  # another story. Kept next to the live tests so it is picked up by renaming
  # once that change lands; never auto-run.
  case "$tk" in *.blocked.tk) continue;; esac
  name=$(basename "$tk" .tk)
  echo "=== $name ==="

  # Compile to LLVM IR
  "$TOKE" --allow-all --emit-llvm --out "$WORK/${name}.ll" "$tk" 2>/dev/null
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
    -x ir "$WORK/${name}.ll" \
    $DEP_ARGS \
    -o "$WORK/${name}_bin" -lm -lz 2>/dev/null

  if [ $? -ne 0 ]; then echo "  LINK ERROR"; TOTAL_FAIL=$((TOTAL_FAIL+1)); continue; fi

  # Run and count pass/fail.
  # 131.45: three per-test fixtures, all optional, beside the .tk:
  #   <name>.stdin     fed to stdin; every other test gets /dev/null, so no
  #                    test reads the caller's terminal (test_io_eof read
  #                    nothing and failed whenever the suite ran from make);
  #   <name>.expected  stdout must equal it byte for byte (one PASS/FAIL);
  #   the exit status  always counts: a non-zero exit with no FAIL line is a
  #                    FAIL (a SIGSEGV used to tally as "0 failed"), and a
  #                    test that prints no PASS/FAIL line and has no
  #                    .expected is judged by its exit status alone.
  input=/dev/null
  [ -f "$DIR/${name}.stdin" ] && input="$DIR/${name}.stdin"
  "$WORK/${name}_bin" < "$input" > "$WORK/${name}.out" 2> "$WORK/${name}.err"
  rc=$?
  output=$(cat "$WORK/${name}.out" "$WORK/${name}.err")
  pass=$(printf '%s\n' "$output" | grep -c "^PASS")
  fail=$(printf '%s\n' "$output" | grep -c "^FAIL")
  printf '%s\n' "$output" | grep "FAIL"
  if [ -f "$DIR/${name}.expected" ]; then
    if cmp -s "$DIR/${name}.expected" "$WORK/${name}.out"; then
      pass=$((pass+1))
    else
      echo "FAIL stdout differs from ${name}.expected"
      diff "$DIR/${name}.expected" "$WORK/${name}.out" | head -5
      fail=$((fail+1))
    fi
  fi
  if [ "$rc" -ne 0 ] && [ "$fail" -eq 0 ]; then
    echo "FAIL exit status $rc with no FAIL line"
    fail=1
  elif [ "$pass" -eq 0 ] && [ "$fail" -eq 0 ]; then
    pass=1                      # exit 0, nothing printed to judge: exit status
  fi
  echo "  $pass passed, $fail failed"
  TOTAL_PASS=$((TOTAL_PASS+pass))
  TOTAL_FAIL=$((TOTAL_FAIL+fail))

  rm -f "$WORK/${name}.ll" "$WORK/${name}_bin" "$WORK/${name}.out" "$WORK/${name}.err"
done

echo ""
echo "=== TOTAL: $TOTAL_PASS passed, $TOTAL_FAIL failed ==="
exit $TOTAL_FAIL
