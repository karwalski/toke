---
title: Binary Size and Memory Footprint
slug: binary-size
section: compiler
order: 1
---

Measured on Mac Studio M4 Max, macOS 26.x, Apple Clang 17.x, arm64.
Date: 2026-04-04.

---

## tkc Compiler Binary

| Metric | Value |
|--------|-------|
| Raw (with debug symbols) | 206,720 bytes (202 KB) |
| Stripped | 185,888 bytes (182 KB) |
| Architecture | Mach-O 64-bit arm64 |

The compiler is a single static binary with no runtime dependencies beyond libc and clang (for backend).

---

## Compiled Toke Program Sizes

All programs compiled with `tkc <file>.tk` (default -O1 via clang backend).

| Program | Raw (bytes) | Stripped (bytes) | Lines of .tk |
|---------|-------------|------------------|--------------|
| fib_recursive | 34,944 | 34,232 | 10 |
| nested_loops | 34,944 | 34,232 | 11 |
| sum_array | 35,048 | 34,240 | 17 |
| collatz | 35,336 | 34,240 | 15 |
| deep_recursion | 35,072 | 34,248 | 12 |
| fib_iterative | 35,120 | 34,248 | 13 |
| binary_search | 35,264 | 34,248 | 22 |
| prime_sieve | 35,176 | 34,248 | 18 |
| gcd_euler | 35,480 | 34,272 | 30 |
| struct_ops | 35,784 | 34,288 | 25 |
| chained_calls | 35,760 | 34,312 | 28 |
| large_expr | 35,864 | 34,256 | 20 |

**Range:** 34.2 -- 34.3 KB stripped across all 12 benchmarks.

---

## Comparison with C Equivalents

| Program | Toke stripped | C stripped | Overhead |
|---------|-------------|-----------|----------|
| nested_loops | 34,232 | 33,440 | +792 bytes (+2.4%) |

The ~800 byte overhead comes from the toke runtime stubs (`tk_runtime_init`, `tk_overflow_trap`, stack probe attributes). Actual user code compiles to comparable size since both go through LLVM.

---

## Compiler Memory Footprint

Measured with `/usr/bin/time -l` during compilation:

| Program | Peak RSS |
|---------|----------|
| gcd_euler.tk (30 lines) | 45.9 MB |
| large_expr.tk (20 lines) | 45.8 MB |

The ~46 MB baseline is dominated by clang/LLVM backend startup. The tkc frontend itself (lexer through type checker) uses negligible additional memory — the arena allocator keeps all AST nodes in a single contiguous block.

---

## Notes

- Stripped binary sizes cluster tightly around 34 KB because the Mach-O overhead and runtime stubs dominate at this program scale. Larger programs would show more variation.
- The tkc compiler at 182 KB stripped is extremely small for a compiler that produces native code (via LLVM).
- No dynamic linking beyond system libc — all toke runtime support is statically linked.
