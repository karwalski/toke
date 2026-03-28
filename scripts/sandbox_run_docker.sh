#!/bin/bash
# sandbox_run_docker.sh — run a toke binary inside a Docker container
# Usage: sandbox_run_docker.sh <binary> [args...]
# Exit codes: 0=success, 1=program error, 124=timeout
set -euo pipefail
BINARY="${1:?Usage: sandbox_run_docker.sh <binary>}"
shift
TIMEOUT=10
WORKDIR=$(mktemp -d /tmp/toke-run-XXXXXX)
cp "$BINARY" "$WORKDIR/program"
chmod +x "$WORKDIR/program"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT
timeout "$TIMEOUT" docker run --rm \
    --network none \
    --read-only \
    --tmpfs /tmp:size=64m \
    --memory 256m \
    --cpus 1 \
    -v "$WORKDIR:/run:ro" \
    --security-opt no-new-privileges \
    --env-file /dev/null \
    alpine:3.19 \
    /run/program "$@" || EXIT=$?
EXIT=${EXIT:-0}
if [ "$EXIT" -eq 124 ]; then
    echo "SANDBOX: timeout after ${TIMEOUT}s" >&2
    exit 124
fi
exit "$EXIT"
