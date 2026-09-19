#!/bin/bash
# vecstore_binding.sh — story 136.4: end-to-end check of the std.vecstore binding.
#
# test/stdlib/test_vecstore.c exercises the C core directly, in one process,
# and passed throughout the period when the module was unusable. This script
# exercises what a consumer actually gets: a .tk program compiled and linked by
# tkc, with the write and the read in SEPARATE PROCESSES.
#
# That separation is the entire point. Before 136.4 the persistence layer in
# vecstore.c was complete -- collection_save, collection_load, the "TKVC" file
# format, all of it -- but its only caller was vecstore_close(), and
# tk_vecstore_close_w did not exist. A toke consumer therefore had no reachable
# path to disk: upserts accumulated in memory, count() would have agreed they
# were there had count() existed, and the process exited having written nothing.
# A same-process round trip cannot tell that apart from a working store, which
# is why this harness re-execs between the write and the read.
#
# Exit 0 = pass, 1 = fail.

set -u

TKC="${TKC:-./toke}"
STDLIB="${TKC_STDLIB_DIR:-$PWD/src/stdlib}"
SRC="test/stdlib/vecstore_roundtrip.tk"
TMP="$(mktemp -d)"
BIN="$TMP/vecstore_roundtrip"
DATA="$TMP/data"

export TKC_STDLIB_DIR="$STDLIB"
export VSDIR="$DATA"

pass=0
fail=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

mkdir -p "$DATA"

check() { # check <label> <phase>
  local label="$1" phase="$2" out rc
  out=$(VSPHASE="$phase" "$BIN" 2>&1); rc=$?
  if [ "$rc" -eq 0 ]; then
    printf "  PASS %-38s %s\n" "$label" "$(echo "$out" | head -1)"
    pass=$((pass+1))
  else
    printf "  FAIL %-38s (exit=%d) %s\n" "$label" "$rc" "$out"
    fail=$((fail+1))
  fi
}

echo "std.vecstore binding (136.4) — $DATA"

if ! $TKC "$SRC" -o "$BIN" 2>&1; then
  echo "  FAIL compile+link"
  exit 1
fi
echo "  PASS compile+link"
pass=$((pass+1))

check "write + close (process 1)" write

# The assertion the module could not previously satisfy: a file exists at all.
if [ -s "$DATA/docs.vecs" ]; then
  printf "  PASS %-38s %s bytes\n" "collection reached disk" "$(wc -c < "$DATA/docs.vecs" | tr -d ' ')"
  pass=$((pass+1))
else
  echo "  FAIL collection reached disk           (no $DATA/docs.vecs — the 136.4 defect)"
  fail=$((fail+1))
fi

check "read back (process 2, never saw write)" read
check "min_score filters (process 3)"          filter
check "delete (process 4)"                     delete
check "delete persisted (process 5)"           afterdelete
check "empty collection returns @() not null"  empty

# Known gap, deliberately not asserted here: comparing an optional's null
# sentinel with `==` segfaults (127.83). std.vecstore returns no ?(T) today, so
# this harness does not trip it, but vecstore.collection is declared
# VecCollection!VecErr and a future error-path test must use a null-safe probe
# rather than `== none`.
echo "  NOTE ?(T) none-sentinel comparison segfaults — see 127.83"

echo "---"
echo "$pass passed, $fail failed"
test "$fail" -eq 0
