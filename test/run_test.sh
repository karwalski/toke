#!/bin/sh
# run_test.sh — run a test binary with a wall-clock timeout.
# Usage: run_test.sh SECS BINARY [args...]
# Uses perl alarm() which survives exec, so the timeout applies to the child
# binary.  Default action for SIGALRM is process termination.
# Exits with the binary's exit code, or non-zero if the timeout fires.
SECS=${1:-180}
shift
exec perl -e '
    my $t = shift @ARGV;
    alarm($t);
    exec @ARGV or die "exec: $!";
' -- "$SECS" "$@"
