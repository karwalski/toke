---
title: Token Comparison
slug: token-comparison
section: reference
order: 20
---

# Token Comparison: toke vs Python vs Go

All counts measured with real tokenizers. toke uses a purpose-built 16K BPE trained on 25,953 toke programs + 698 loke production modules. Python and Go use OpenAI's cl100k_base (100K vocab). toke code is also shown with cl100k for fair same-tokenizer comparison.

## Summary

| Metric | Average |
|--------|---------|
| toke (BPE) vs Python (cl100k) | **31% fewer tokens** |
| toke (BPE) vs Go (cl100k) | **48% fewer tokens** |
| toke (cl100k) vs Python (cl100k) | -80% fewer tokens |
| Examples tested | 42 |

## Full Results

| # | Task | Category | toke BPE | toke cl100k | Python cl100k | Go cl100k | vs Python | vs Go |
|---|------|----------|----------|-------------|---------------|-----------|-----------|-------|
| 1 | Hello World | Basics | 11 | 26 | 5 | 18 | -120% | +39% |
| 2 | Absolute Value | Basics | 9 | 29 | 15 | 20 | +40% | +55% |
| 3 | Max of Two | Basics | 11 | 28 | 15 | 20 | +27% | +45% |
| 4 | Is Even | Basics | 10 | 24 | 14 | 16 | +29% | +38% |
| 5 | Factorial (recursive) | Basics | 14 | 31 | 25 | 26 | +44% | +46% |
| 6 | Fibonacci (recursive) | Basics | 14 | 34 | 27 | 29 | +48% | +52% |
| 7 | Fibonacci (iterative) | Basics | 14 | 52 | 36 | 42 | +61% | +67% |
| 8 | Sum Array | Basics | 8 | 44 | 25 | 27 | +68% | +70% |
| 9 | Max in Array | Basics | 17 | 54 | 33 | 37 | +48% | +54% |
| 10 | Reverse Array | Basics | 19 | 52 | 10 | 55 | -90% | +65% |
| 11 | Binary Search | Intermediate | 46 | 99 | 80 | 75 | +43% | +39% |
| 12 | Bubble Sort | Intermediate | 32 | 88 | 62 | 78 | +48% | +59% |
| 13 | FizzBuzz | Intermediate | 45 | 96 | 68 | 73 | +34% | +38% |
| 14 | GCD (Euclidean) | Intermediate | 21 | 59 | 24 | 28 | +12% | +25% |
| 15 | Is Prime | Intermediate | 18 | 62 | 49 | 45 | +63% | +60% |
| 16 | Power Function | Intermediate | 11 | 46 | 27 | 34 | +59% | +68% |
| 17 | Count Occurrences | Intermediate | 19 | 55 | 32 | 35 | +41% | +46% |
| 18 | Sum of Digits | Intermediate | 24 | 65 | 18 | 44 | -33% | +45% |
| 19 | Nth Prime | Intermediate | 39 | 114 | 90 | 83 | +57% | +53% |
| 20 | Matrix 2x2 Multiply | Intermediate | 67 | 103 | 75 | 84 | +11% | +20% |
| 21 | Filter Evens | Advanced | 18 | 63 | 23 | 43 | +22% | +58% |
| 22 | Map: Square Each | Advanced | 13 | 52 | 14 | 36 | +7% | +64% |
| 23 | Reduce: Product | Advanced | 13 | 46 | 26 | 27 | +50% | +52% |
| 24 | Merge Sorted Arrays | Advanced | 60 | 152 | 76 | 96 | +21% | +38% |
| 25 | Insertion Sort | Advanced | 37 | 90 | 71 | 70 | +48% | +47% |
| 26 | Selection Sort | Advanced | 38 | 89 | 47 | 77 | +19% | +51% |
| 27 | Min of Array | Basics | 17 | 54 | 33 | 37 | +48% | +54% |
| 28 | Clamp | Basics | 15 | 41 | 18 | 30 | +17% | +50% |
| 29 | Swap | Basics | 11 | 24 | 11 | 18 | +0% | +39% |
| 30 | Average | Basics | 13 | 49 | 13 | 31 | +0% | +58% |
| 31 | Contains | Basics | 10 | 45 | 11 | 30 | +9% | +67% |
| 32 | Index Of | Intermediate | 13 | 48 | 21 | 32 | +38% | +59% |
| 33 | Two Sum | Advanced | 25 | 70 | 49 | 65 | +49% | +62% |
| 34 | Flatten 2D | Advanced | 26 | 72 | 19 | 34 | -37% | +24% |
| 35 | Dot Product | Advanced | 17 | 52 | 21 | 33 | +19% | +48% |
| 36 | Unique (dedup sorted) | Advanced | 25 | 76 | 42 | 62 | +40% | +60% |
| 37 | Range (0 to n-1) | Intermediate | 15 | 42 | 11 | 32 | -36% | +53% |
| 38 | Palindrome (number) | Intermediate | 22 | 59 | 20 | 43 | -10% | +49% |
| 39 | LCM | Intermediate | 30 | 81 | 22 | 48 | -36% | +38% |
| 40 | HTTP Hello Server | Advanced | 35 | 53 | 58 | 50 | +40% | +30% |
| 41 | JSON Parse + Sum | Advanced | 29 | 66 | 20 | 65 | -45% | +55% |
| 42 | Struct + Method | Advanced | 45 | 59 | 61 | 56 | +26% | +20% |

## Methodology

- **toke BPE:** 16,384-token vocabulary trained on 25,953 v0.3 corpus records + 698 loke production .tk files
- **cl100k_base:** OpenAI's 100,277-token vocabulary (used by GPT-4, Claude via tiktoken)
- All toke examples are single-line (optimal for BPE). Multi-line adds ~50% more tokens due to whitespace
- Python and Go examples are idiomatic (not golfed), representing what a developer would actually write
- toke programs that look larger than Python (e.g., `arr.get(i)` vs `arr[i]`) still tokenise shorter because BPE merges common toke patterns

## Where toke wins most

toke's advantage is largest on programs that use:
- Multiple function signatures (`:i64):i64{` merges to few tokens)
- Standard library imports (`i=j:std.json` = 1 token)
- Struct types and construction (`$point{x:1;y:2}` = compact)
- HTTP/JSON patterns (very common in corpus, heavily merged)

## Where Python wins

Python is more compact when:
- The task is a one-liner with built-in functions (`sum()`, `len()`, `range()`, list comprehensions)
- No imports/module boilerplate needed
- String operations that Python handles natively but toke requires explicit loops

## Interactive Token Visualisation

See the [interactive token visualiser](/tokens.html) to see exactly how each token boundary falls, with colour-highlighted tokens for side-by-side comparison.