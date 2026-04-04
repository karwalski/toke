# toke vs C Compiled Binary Benchmarks

Benchmark suite comparing native binaries produced by the toke compiler (`tkc`)
against equivalent C programs compiled with the system C compiler.

## Programs

| # | Benchmark | Description |
|---|-----------|-------------|
| 1 | fib_recursive | Recursive Fibonacci (fib(35)) — measures function call overhead |
| 2 | fib_iterative | Iterative Fibonacci in a loop (1M iterations) — loop + mutation |
| 3 | sum_array | Sum a 50-element array (100K iterations) — array access patterns |
| 4 | nested_loops | 1000x1000 nested loop with arithmetic — tight loop performance |
| 5 | binary_search | Binary search on a sorted array (1M searches) — branching |
| 6 | prime_sieve | Trial-division primality check up to 50K — integer arithmetic |
| 7 | deep_recursion | Ackermann function ack(3,10) — deep recursion / stack pressure |
| 8 | struct_ops | Vec3 add/dot/scale (1M iterations) — struct pass-by-value |
| 9 | large_expr | Large polynomial expression (5M iterations) — expression codegen |
| 10 | chained_calls | 5-deep function call chain (10M iterations) — call/return overhead |
| 11 | collatz | Collatz sequence lengths for 1..500K — branch-heavy loops |
| 12 | gcd_euler | Euler totient via GCD for 2..10K — nested loops + integer ops |

## How to Run

```bash
# Make the runner executable (one time)
chmod +x bench/run_bench.sh

# Run with default 10 iterations per benchmark
./bench/run_bench.sh

# Run with custom iteration count
./bench/run_bench.sh 5
```

Results are written to `bench/results/` as timestamped markdown files and also
printed to stdout.

## What is Measured

- **Wall time** (median of N runs): captured via `/usr/bin/time -lp` on macOS.
- **Binary size**: output binary size in bytes via `stat`.
- **Peak RSS**: maximum resident set size in KB via `/usr/bin/time -lp`.

## Compilation Flags

Both compilers are invoked with `-O2` optimisation:

- toke: `tkc -O2 source.tk --out binary`
- C: `cc -O2 -o binary source.c`

## Caveats

1. **LLVM backend**: toke compiles through LLVM, so at `-O2` both compilers
   benefit from the same LLVM optimisation passes. Differences reflect
   frontend codegen quality (IR generation, calling conventions, memory layout)
   rather than backend optimisation differences.

2. **No modulo operator**: toke v0.1 lacks `%`, so modulo is emulated as
   `n - n/d*d`. The C versions use native `%` — this is a fair comparison
   since it reflects what a toke programmer would actually write.

3. **No array mutation**: toke v0.1 does not support `arr[i] = val`, so
   sorting benchmarks are replaced with alternatives (Collatz, GCD/Euler).

4. **Struct passing**: toke passes structs by value. The C versions do the
   same for a fair comparison, though real C code might use pointers.

5. **Process exit code**: toke programs return i64 from main (used as
   process exit code). C programs mask results with `& 0xFF` to avoid
   exit-code truncation issues. This does not affect timing.

6. **macOS only**: The runner uses `/usr/bin/time -lp` and `stat -f%z`,
   both macOS-specific. Linux support would require `/usr/bin/time -v`
   and `stat --printf=%s`.

7. **Cold cache**: The first run of each binary may be slower due to
   page faults. The median over N runs mitigates this.
