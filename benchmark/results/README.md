# stdlib Benchmark Baseline

**Date:** 2026-03-28
**Host:** Apple M-series, macOS (Darwin 25.x, arm64)
**Build flags:** `-O2`
**Compiler:** Apple clang (system default)

## What the numbers mean

Each line: `BENCH <function> <N_iters> <total_ms> <avg_us/iter>`

- `N_iters` — number of times the function was called in the timed loop
- `total_ms` — wall-clock time for all iterations (monotonic clock)
- `avg_us/iter` — average microseconds per call

`c_native.*[baseline]` lines show equivalent C-native operations for comparison.

## Regression threshold

Any release that shows **> 20% regression** on any benchmark line vs this
file is blocked until the regression is explained or the baseline is updated
with a documented reason (e.g. intentional algorithm change).

To update the baseline after an intentional change:

```
make bench > benchmark/results/baseline.txt
```

Then commit with a message explaining the change.

## Notes on specific benchmarks

- **file.write+read+delete**: Dominated by filesystem I/O (~513 µs/iter on
  this host). Performance varies with OS disk cache state. 20% threshold
  applies relative to a warm-cache run.

- **process.spawn+wait(true)**: Fork/exec overhead (~9 ms/iter). Expected to
  vary by OS load. Not a hot path in the stdlib.

- **log.info**: Output redirected to `/dev/null` during benchmark. Real
  stderr write cost will be higher when logs are consumed.

- **c_native.sha256_memcpy[baseline]**: The compiler optimises the memcpy
  loop to near-zero. The baseline is illustrative, not a strict comparison.
