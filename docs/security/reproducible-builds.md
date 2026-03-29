# Reproducible Builds

tkc supports reproducible builds: the same source tree, compiler, and
environment produce bit-identical binaries. This complements the SBOM
(story 3.7.1) and cosign signing (story 3.7.2) to form a complete
supply-chain verification pipeline.

## Quick verification

```sh
make repro-check
```

This target builds tkc twice with a pinned `SOURCE_DATE_EPOCH`, then
compares SHA-256 hashes of all object files. It exits non-zero if any
object file differs between the two builds.

On Linux the final linked binary is also compared. On macOS the Apple
linker (`ld64`) embeds a random `LC_UUID` in every Mach-O binary, so
the final executables will differ even when all inputs are identical.
`make repro-check` reports this as informational rather than a failure,
because the compilation itself (which is the supply-chain-relevant
step) is fully deterministic.

## Compiler flags

The following flags are added to every compilation step via
`REPRO_FLAGS` in the Makefile:

| Flag | Purpose |
|------|---------|
| `-frandom-seed=tkc` | Makes GCC/Clang internal hashes deterministic instead of seeding from PID, timestamps, or random data. |
| `-ffile-prefix-map=$(CURDIR)=.` | Rewrites absolute file paths to relative `.` in debug info and `__FILE__` expansions, so builds in different directories produce identical output. |

### Platform notes

- **Linux (CI target):** ELF linking is deterministic with modern `ld`.
  Both object files and the final binary are compared.
- **macOS:** The Apple linker embeds a random `LC_UUID`. Object files
  are verified as identical; the final binary difference is expected and
  reported as informational.

## SOURCE_DATE_EPOCH

`SOURCE_DATE_EPOCH` is a [standardised environment variable](https://reproducible-builds.org/specs/source-date-epoch/)
that tells compilers to use a fixed timestamp for `__DATE__`, `__TIME__`,
and `__TIMESTAMP__` macros.

- **CI:** The CI workflow pins `SOURCE_DATE_EPOCH` to the commit
  timestamp (`git log -1 --format=%ct`), so every build of the same
  commit embeds the same date strings.
- **Local `make repro-check`:** Uses a fixed epoch (`1700000000`) for
  both builds to guarantee identical output regardless of wall-clock
  time.
- **Regular `make`:** Defaults `SOURCE_DATE_EPOCH` to `0` via the
  Makefile (`export SOURCE_DATE_EPOCH ?= 0`). Override it in your
  environment if you prefer a different epoch.

## What is NOT covered

- **Cross-platform reproducibility.** Two different operating systems or
  compiler versions may produce different binaries. Reproducibility is
  guaranteed within the same platform and toolchain.
- **Debug builds vs release builds.** Different `CFLAGS` (e.g., `-O2
  -DNDEBUG` for releases) naturally produce different binaries. The
  guarantee is: same flags, same source, same compiler produces same
  output.

## CI integration

The `repro` job in `.github/workflows/ci.yml` runs `make repro-check`
on every push and pull request. If the two builds produce different
hashes the job fails, blocking merge.
