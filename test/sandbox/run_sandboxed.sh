#!/bin/sh
# run_sandboxed.sh — run a test binary in an isolated temp directory.
# Usage: run_sandboxed.sh BINARY [args...]
# Creates a per-process temp sandbox, sets HOME to it, runs the binary
# with a 10-second timeout, then cleans up regardless of outcome.
set -u

TIMEOUT=10
SANDBOX="/tmp/toke-test-$$"

usage() {
  echo "Usage: $0 BINARY [args...]" >&2
  exit 1
}

[ $# -lt 1 ] && usage

BINARY="$1"
shift

if [ ! -x "$BINARY" ]; then
  echo "error: $BINARY is not executable" >&2
  exit 127
fi

# Create sandbox
mkdir -p "$SANDBOX" || exit 1

# Cleanup function
cleanup() {
  rm -rf "$SANDBOX" 2>/dev/null
}

# Trap signals for cleanup
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

# Run the test binary with HOME pointing at the sandbox
export HOME="$SANDBOX"

# Use perl alarm for a reliable wall-clock timeout
perl -e '
  my $t = shift @ARGV;
  alarm($t);
  exec @ARGV or die "exec: $!";
' -- "$TIMEOUT" "$BINARY" "$@"

RC=$?
exit $RC
