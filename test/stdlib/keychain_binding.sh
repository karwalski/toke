#!/bin/bash
# keychain_binding.sh — story 136.3: end-to-end check of the std.keychain binding.
#
# test/stdlib/test_keychain.c exercises the C core directly. This script
# exercises what a consumer actually gets: a .tk program compiled and linked by
# tkc. That is the part that was broken — the stdlib_deps entry for keychain
# carried empty link flags, so every consumer failed with ~20 undefined _kSec*
# and _CF* symbols, and keychain.isavailable had no _w wrapper at all.
#
# The write and read phases run as SEPARATE PROCESSES of the same binary, so a
# pass proves the secret reached the OS credential store and came back out of
# it — not that it survived in process memory. (Epic 136.4 records a
# neighbouring module that appears to accept writes and persists nothing; a
# single-process round trip would not have caught that.)
#
# Same binary, so the same code identity owns the keychain ACL it created and
# no authorisation dialog is raised. Each run uses a fresh service name, so it
# never touches an entry created by an earlier build.
#
# Exit 0 = pass or skip (no credential store on this platform), 1 = fail.

set -u

TKC="${TKC:-./toke}"
STDLIB="${TKC_STDLIB_DIR:-$PWD/src/stdlib}"
SRC="test/stdlib/keychain_roundtrip.tk"
BIN="$(mktemp -d)/keychain_roundtrip"
SVC="toke.136.3.$$.$(date +%s)"
ACCT="probe-account"

export TKC_STDLIB_DIR="$STDLIB"
export TKKCSERVICE="$SVC"
export TKKCACCOUNT="$ACCT"

pass=0
fail=0

cleanup() {
  # Never leave a probe entry behind, whatever happened above.
  [ -x "$BIN" ] && TKKCPHASE=cleanup "$BIN" >/dev/null 2>&1
  rm -rf "$(dirname "$BIN")"
}
trap cleanup EXIT

check() { # check <label> <expected-exit> <phase>
  local label="$1" want="$2" phase="$3" out rc
  out=$(TKKCPHASE="$phase" "$BIN" 2>&1); rc=$?
  if [ "$rc" -eq "$want" ]; then
    printf "  PASS %-34s %s\n" "$label" "$out"
    pass=$((pass+1))
  else
    printf "  FAIL %-34s (exit=%d want=%d) %s\n" "$label" "$rc" "$want" "$out"
    fail=$((fail+1))
  fi
}

echo "std.keychain binding (136.3) — service $SVC"

if ! $TKC "$SRC" -o "$BIN" 2>&1; then
  echo "  FAIL compile+link                    (this is the 136.3 defect)"
  exit 1
fi
echo "  PASS compile+link"
pass=$((pass+1))

# Platforms with no credential store report SKIP and exit 0 from every phase.
if TKKCPHASE=all "$BIN" 2>&1 | grep -q '^SKIP'; then
  echo "  SKIP no OS credential store on this platform"
  exit 0
fi

check "write (process 1)"            0 write
check "read back (process 2)"        0 check
check "delete (process 3)"           0 cleanup
check "absent after delete"          0 absent
check "full lifecycle in one process" 0 all

# Known gap, deliberately not asserted here: keychain.get is declared ?(str) and
# returns the documented NULL on a miss, but comparing that none-sentinel with
# `==` segfaults the consumer (exit 139) because str equality does not guard a
# null operand. str.len() on it is safe, which is what "absent" uses. This is a
# language-level hole in ?(T) handling, not a keychain one -- tls.read,
# tls.peercert, securemem.read and mdns.resolve all return the same sentinel.
# Reported under Epic 136 for a story of its own; do not "fix" it in this module
# by returning "" instead, which would make an absent secret indistinguishable
# from an empty one.
echo "  NOTE ?(str) none-sentinel comparison segfaults -- see header, needs a story"

echo "---"
echo "$pass passed, $fail failed"
test "$fail" -eq 0
