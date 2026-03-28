#!/bin/bash
# sandbox_run.sh — run a toke binary inside macOS sandbox-exec
# Usage: sandbox_run.sh <binary> [args...]
# Exit codes: 0=success, 1=program error, 124=timeout, 125=sandbox violation

set -euo pipefail

BINARY="${1:?Usage: sandbox_run.sh <binary>}"
shift
TIMEOUT=10
WORKDIR=$(mktemp -d /tmp/toke-run-XXXXXX)

PROFILE_FILE=$(mktemp /tmp/toke-sandbox-XXXXXX.sb)

cleanup() {
    rm -rf "$WORKDIR"
    rm -f "$PROFILE_FILE"
}
trap cleanup EXIT

# macOS sandbox profile
cat > "$PROFILE_FILE" <<SANDBOX
(version 1)
(deny default)
(allow process-exec (literal "${BINARY}"))
(allow file-read*
    (subpath "/usr/lib")
    (subpath "/usr/local/lib")
    (subpath "/System")
    (subpath "/private/tmp/toke-run"))
(allow file-write*
    (subpath "/private/tmp/toke-run"))
(deny network*)
SANDBOX

# Run with timeout; capture exit code without triggering set -e
timeout "$TIMEOUT" sandbox-exec -f "$PROFILE_FILE" env -i PATH=/usr/bin:/bin "$BINARY" "$@" || EXIT=$?
EXIT=${EXIT:-0}

if [ "$EXIT" -eq 124 ]; then
    echo "SANDBOX: timeout after ${TIMEOUT}s" >&2
    exit 124
fi
exit "$EXIT"
