# Benchmark Programs

Non-trivial toke programs for benchmarking tkc-compiled binaries against
equivalent C and Rust implementations.

## Programs

### sieve.tk -- Prime Counting (Trial Division)

Finds all primes below 100,000 by trial division against accumulated primes,
repeated 5 times.

Exercises: array growth (push), nested iteration, conditional branching,
modular arithmetic.

Expected output: **9592 primes** below 100,000 (verified against known count).

The trial division approach is intentionally compute-heavy: each candidate is
tested against all known primes up to its square root, making this O(n * sqrt(n) / ln(n)).

### matrix.tk -- Matrix Multiplication

Multiplies two 200x200 matrices of f64 values, repeated 3 times.

Exercises: nested loops (O(n^3)), floating-point arithmetic, flat-array
indexing, sequential array construction via push.

Output: checksum of corner and centre elements for correctness verification.

## Building and Running

```bash
# compile
tkc test/benchmark/sieve.tk -o build/bench_sieve
tkc test/benchmark/matrix.tk -o build/bench_matrix

# run
./build/bench_sieve
./build/bench_matrix

# with timing (if elapsed output is not available)
time ./build/bench_sieve
time ./build/bench_matrix
```

## Expected Runtime

Each benchmark should run for approximately 2-5 seconds on modern hardware
(M4 Max or equivalent). If they finish too quickly, increase the `iterations`
constant in the source. If they run too long, decrease `limit` or `n`.

## Comparison Testing

To compare against C or Rust, implement the same algorithm and measure:

```bash
# C reference
gcc -O2 -o bench_sieve_c reference/sieve.c
time ./bench_sieve_c

# Rust reference
rustc -O -o bench_sieve_rs reference/sieve.rs
time ./bench_sieve_rs

# toke
time ./build/bench_sieve
```

Key metrics to compare:
- Wall-clock time (via `time` command or printed elapsed seconds)
- Peak memory usage (via `/usr/bin/time -l` on macOS)

## Notes

- Both programs print their result to stdout to prevent dead-code elimination
  by the compiler.
- The prime benchmark verifies its output against the known prime count (9592).
- The matrix benchmark prints a checksum; compare this value across all
  implementations to confirm they compute the same result.
- Arrays in toke are append-only (push), so both algorithms build results
  sequentially rather than using random-access writes.
- `time.now()` returns epoch seconds; for sub-second precision use the
  shell `time` command.
