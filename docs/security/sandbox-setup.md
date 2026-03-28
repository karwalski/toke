# Sandbox Setup Guide

**Scope:** Reproducible sandbox configuration for the toke differential test harness on Mac Studio (macOS) and Linux.
**Last updated:** 2026-03-28

---

## 1. macOS Setup (sandbox-exec)

`sandbox-exec` is a built-in macOS tool; no installation is required.

Verify it is available:

```bash
sandbox-exec -n no-network true && echo "sandbox-exec OK"
```

The script `scripts/sandbox_run.sh` writes a temporary `.sb` profile that:
- Denies all actions by default
- Allows file reads under `/usr/lib`, `/usr/local/lib`, `/System`, and `/private/tmp/toke-run`
- Allows file writes only under `/private/tmp/toke-run`
- Denies all network access
- Passes a clean environment (`env -i`) to the child process

---

## 2. Docker Setup (Linux or macOS fallback)

Pull the base image:

```bash
docker pull alpine:3.19
```

The script `scripts/sandbox_run_docker.sh` runs the binary with:
- `--network none` — outbound and inbound network disabled
- `--read-only` — root filesystem read-only
- `--tmpfs /tmp:size=64m` — writable scratch space capped at 64 MB
- `--memory 256m` — memory cap
- `--cpus 1` — single CPU
- `--security-opt no-new-privileges` — prevents privilege escalation
- `--env-file /dev/null` — empty environment (no host env vars inherited)

---

## 3. Sandbox Method Selection

Use `sandbox_run.sh` on macOS and `sandbox_run_docker.sh` on Linux. To auto-select:

```bash
case "$(uname -s)" in
    Darwin) RUNNER=scripts/sandbox_run.sh ;;
    Linux)  RUNNER=scripts/sandbox_run_docker.sh ;;
    *) echo "Unsupported platform" >&2; exit 1 ;;
esac
"$RUNNER" /path/to/binary
```

Both scripts share identical exit code semantics: 0 success, 1 program error, 124 timeout.

---

## 4. Outbound Network Block Test

Compile a program that attempts an outbound HTTP request, then verify the sandbox blocks it:

```bash
cat > /tmp/net_test.c << 'EOF'
#include <stdlib.h>
int main() { return system("curl -s http://example.com > /dev/null 2>&1"); }
EOF
cc -o /tmp/net_test /tmp/net_test.c
./scripts/sandbox_run.sh /tmp/net_test
# Expected: non-zero exit (network denied by sandbox)
echo "Exit: $?"
```

---

## 5. Timeout Enforcement Test

Compile an infinite loop and verify the sandbox kills it after 10 seconds:

```bash
cat > /tmp/loop.c << 'EOF'
int main() { for(;;); }
EOF
cc -o /tmp/loop /tmp/loop.c
./scripts/sandbox_run.sh /tmp/loop
# Expected: exit 124 (timeout)
echo "Exit: $?"
```

---

## 6. Corpus Pipeline Integration

The corpus harness calls the sandbox runner instead of executing binaries directly:

```bash
# Discard entry and log on timeout or sandbox violation
"$RUNNER" "$BINARY" < "$INPUT" > "$OUTPUT" 2>"$STDERR_LOG" || {
    EXIT=$?
    echo "$(date -u +%Y-%m-%dT%H:%M:%SZ) DISCARD task_id=${TASK_ID} exit=${EXIT}" >> corpus/run.log
    # Entry is not written to corpus
    continue
}
```

Entries that produce exit code 124 (timeout) or non-zero are discarded and logged; they are
not retried, as per Story 1.8.2 acceptance criteria.
