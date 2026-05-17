#!/bin/bash
# Run standalone .tk test files: compile with toke + llc + cc, then execute.

TKC="./toke"
LLC="/opt/homebrew/opt/llvm/bin/llc"
STDLIB="src/stdlib"
CFLAGS="-O1 -Wno-implicit-function-declaration -I $STDLIB"
LIBS="-lm"
LINK_SRCS="$STDLIB/tk_runtime.c $STDLIB/args.c $STDLIB/str.c $STDLIB/str_glue.c \
  $STDLIB/collections.c $STDLIB/collections_glue.c \
  $STDLIB/crypto.c $STDLIB/crypto_glue.c \
  $STDLIB/tk_time.c $STDLIB/time_glue.c \
  $STDLIB/json.c $STDLIB/json_glue.c \
  $STDLIB/file.c $STDLIB/file_glue.c \
  $STDLIB/encoding.c $STDLIB/encoding_glue.c \
  $STDLIB/math.c $STDLIB/math_glue.c \
  $STDLIB/process.c $STDLIB/process_glue.c \
  $STDLIB/env.c $STDLIB/env_glue.c \
  $STDLIB/io_glue.c $STDLIB/path.c $STDLIB/path_glue.c"

pass=0; fail=0; skip=0
for f in "$@"; do
  bn=$(basename "$f" .tk)
  # 1. Codegen
  if ! $TKC --emit-llvm "$f" --out "/tmp/${bn}.ll" 2>/dev/null; then
    printf "  SKIP %-35s (codegen error)\n" "$bn"; skip=$((skip+1)); continue
  fi
  # 2. LLC
  if ! $LLC -O1 -filetype=obj "/tmp/${bn}.ll" -o "/tmp/${bn}.o" 2>/dev/null; then
    printf "  SKIP %-35s (llc error)\n" "$bn"; skip=$((skip+1)); continue
  fi
  # 3. Link
  if ! cc $CFLAGS "/tmp/${bn}.o" $LINK_SRCS $LIBS -o "/tmp/${bn}" 2>/dev/null; then
    printf "  SKIP %-35s (link error)\n" "$bn"; skip=$((skip+1)); continue
  fi
  # 4. Run
  /tmp/${bn} >/dev/null 2>&1; rc=$?
  if [ $rc -eq 0 ]; then
    printf "  PASS %-35s\n" "$bn"; pass=$((pass+1))
  else
    printf "  FAIL %-35s (exit=%d)\n" "$bn" "$rc"; fail=$((fail+1))
  fi
  rm -f "/tmp/${bn}.ll" "/tmp/${bn}.o" "/tmp/${bn}"
done
echo "---"
echo "$pass passed, $fail failed, $skip skipped"
test $fail -eq 0 -a $skip -eq 0
