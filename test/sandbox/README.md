# test/sandbox/

Sandboxed test runner for toke test binaries.

## Usage

```sh
./test/sandbox/run_sandboxed.sh path/to/test_binary [args...]
```

## What it does

1. Creates a temporary directory at `/tmp/toke-test-<PID>`
2. Sets `HOME` to that directory so the test binary cannot touch real user files
3. Runs the binary with a 10-second wall-clock timeout (via `perl alarm()`)
4. Captures the exit code
5. Removes the temp directory on exit (including signal-based termination)
6. Exits with the test binary's exit code

## Signals

- `INT` (Ctrl-C) and `TERM` are caught; the sandbox is cleaned up before exit.
- The `EXIT` trap ensures cleanup even on normal termination.

## Exit codes

| Code | Meaning |
|------|---------|
| 0    | Test passed |
| 1-125 | Test failure (exit code from the binary) |
| 127  | Binary not found or not executable |
| 130  | Interrupted (SIGINT) |
| 142  | Timeout (SIGALRM from perl) |
| 143  | Terminated (SIGTERM) |
