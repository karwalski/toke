---
title: Token Comparison
slug: token-comparison
section: reference
order: 20
---

# Token Comparison: toke vs Python vs Go

> **Requalified 2026-09-19 (stories 132.6 / 132.13).** This page is the v0.3-era N = 42
> dataset that the "52% average token reduction" and "31% fewer tokens than Python"
> headlines came from. Two things were wrong with how it was summarised, and both are
> fixed below.
>
> 1. **The cross-tokenizer summary rows are withdrawn.** "toke (BPE) vs Python (cl100k)"
>    counts the toke side with a **toke-trained** 16K BPE and the Python/Go side with
>    cl100k_base. A tokenizer trained on toke is out of domain on Python, so that number
>    measures its own training bias, not the two languages. Any cross-language figure must
>    use **one** tokenizer on both sides (TEMSpec §2.3).
> 2. **The honest same-tokenizer answer is the opposite sign.** Under cl100k_base on both
>    sides, toke costs **more** tokens than Python: **1.34x [1.22, 1.48]** over the 60
>    Gate-1 tasks (4,787 vs 3,565; N = 60, 2026-09-19,
>    `toke-eval/docs/gate1-60-v04.md`) and 1.30x over the four execution-verified v0.4
>    sample pairs (N = 4, `docs/about/samples-v04.md`).
>
> The per-example counts below are kept as the historical v0.3 record, with every column
> labelled by its tokenizer. The text is **v0.3 syntax, single-line**; it is not canonical
> `tkc --min` v0.4 text, so it is not comparable to any current measurement. Canonical
> numbers and approved wording: `docs/metrics-baseline.md`.

## Summary

| Metric | Metric type (TEMSpec) | Tokenizer(s) | Baseline | N | Value |
|---|---|---|---|---:|---|
| toke vs Python, one shared tokenizer | §2.3 cross-language density | cl100k_base both sides | Python | 42 | **toke costs 1.80x Python** (2,544 vs 1,417 tokens, summed over the table below) |
| toke vs Go, one shared tokenizer | §2.3 cross-language density | cl100k_base both sides | Go | 42 | **toke costs 1.35x Go** (2,544 vs 1,884) |
| Toke-16K vs cl100k_base on the same toke text | §2.2 compression ratio (one text, two tokenizers) | Toke-16K v0.3 vs cl100k_base | toke itself | 42 | **52% fewer tokens** -- *superseded, see below* |
| ~~toke (BPE) vs Python (cl100k)~~ | — | — | — | — | **withdrawn: two different tokenizers** |
| ~~toke (BPE) vs Go (cl100k)~~ | — | — | — | — | **withdrawn: two different tokenizers** |

**On the 52% row:** it is a tokenizer-vs-tokenizer number on identical toke source, never a
comparison with Python, and it does not reconcile with this table -- re-aggregating the 42
rows below gives **61.6%** (sum-ratio), 62.6% (per-task mean) or 63.8% (median) for that
comparison. No artefact in any repo reproduces 52%, so the headline is withdrawn rather
than corrected. (The two withdrawn cross-tokenizer headlines *do* come from here exactly:
Toke-16K-on-toke vs cl100k-on-Python = 31.1%, vs Go = 48.2%.) It is also superseded. On canonical v0.4 `--min` text (N = 2,000
stratified corpus records, 2026-09-18) every shipped toke tokenizer is worse than
cl100k_base -- the 8K SentencePiece needs **15.4% more** tokens -- and `tokenizer_v03.json`
only appears to win because its null `unk_token` silently drops every backslash.

## Full Results

| # | Task | Category | toke (Toke-16K) | toke (cl100k) | Python (cl100k) | Go (cl100k) | toke/Python (cl100k) | toke/Go (cl100k) |
|---|------|----------|----------------:|--------------:|----------------:|------------:|---------------------:|-----------------:|
| 1 | Hello World | Basics | 11 | 26 | 5 | 18 | 5.20x | 1.44x |
| 2 | Absolute Value | Basics | 9 | 29 | 15 | 20 | 1.93x | 1.45x |
| 3 | Max of Two | Basics | 11 | 28 | 15 | 20 | 1.87x | 1.40x |
| 4 | Is Even | Basics | 10 | 24 | 14 | 16 | 1.71x | 1.50x |
| 5 | Factorial (recursive) | Basics | 14 | 31 | 25 | 26 | 1.24x | 1.19x |
| 6 | Fibonacci (recursive) | Basics | 14 | 34 | 27 | 29 | 1.26x | 1.17x |
| 7 | Fibonacci (iterative) | Basics | 14 | 52 | 36 | 42 | 1.44x | 1.24x |
| 8 | Sum Array | Basics | 8 | 44 | 25 | 27 | 1.76x | 1.63x |
| 9 | Max in Array | Basics | 17 | 54 | 33 | 37 | 1.64x | 1.46x |
| 10 | Reverse Array | Basics | 19 | 52 | 10 | 55 | 5.20x | 0.95x |
| 11 | Binary Search | Intermediate | 46 | 99 | 80 | 75 | 1.24x | 1.32x |
| 12 | Bubble Sort | Intermediate | 32 | 88 | 62 | 78 | 1.42x | 1.13x |
| 13 | FizzBuzz | Intermediate | 45 | 96 | 68 | 73 | 1.41x | 1.32x |
| 14 | GCD (Euclidean) | Intermediate | 21 | 59 | 24 | 28 | 2.46x | 2.11x |
| 15 | Is Prime | Intermediate | 18 | 62 | 49 | 45 | 1.27x | 1.38x |
| 16 | Power Function | Intermediate | 11 | 46 | 27 | 34 | 1.70x | 1.35x |
| 17 | Count Occurrences | Intermediate | 19 | 55 | 32 | 35 | 1.72x | 1.57x |
| 18 | Sum of Digits | Intermediate | 24 | 65 | 18 | 44 | 3.61x | 1.48x |
| 19 | Nth Prime | Intermediate | 39 | 114 | 90 | 83 | 1.27x | 1.37x |
| 20 | Matrix 2x2 Multiply | Intermediate | 67 | 103 | 75 | 84 | 1.37x | 1.23x |
| 21 | Filter Evens | Advanced | 18 | 63 | 23 | 43 | 2.74x | 1.47x |
| 22 | Map: Square Each | Advanced | 13 | 52 | 14 | 36 | 3.71x | 1.44x |
| 23 | Reduce: Product | Advanced | 13 | 46 | 26 | 27 | 1.77x | 1.70x |
| 24 | Merge Sorted Arrays | Advanced | 60 | 152 | 76 | 96 | 2.00x | 1.58x |
| 25 | Insertion Sort | Advanced | 37 | 90 | 71 | 70 | 1.27x | 1.29x |
| 26 | Selection Sort | Advanced | 38 | 89 | 47 | 77 | 1.89x | 1.16x |
| 27 | Min of Array | Basics | 17 | 54 | 33 | 37 | 1.64x | 1.46x |
| 28 | Clamp | Basics | 15 | 41 | 18 | 30 | 2.28x | 1.37x |
| 29 | Swap | Basics | 11 | 24 | 11 | 18 | 2.18x | 1.33x |
| 30 | Average | Basics | 13 | 49 | 13 | 31 | 3.77x | 1.58x |
| 31 | Contains | Basics | 10 | 45 | 11 | 30 | 4.09x | 1.50x |
| 32 | Index Of | Intermediate | 13 | 48 | 21 | 32 | 2.29x | 1.50x |
| 33 | Two Sum | Advanced | 25 | 70 | 49 | 65 | 1.43x | 1.08x |
| 34 | Flatten 2D | Advanced | 26 | 72 | 19 | 34 | 3.79x | 2.12x |
| 35 | Dot Product | Advanced | 17 | 52 | 21 | 33 | 2.48x | 1.58x |
| 36 | Unique (dedup sorted) | Advanced | 25 | 76 | 42 | 62 | 1.81x | 1.23x |
| 37 | Range (0 to n-1) | Intermediate | 15 | 42 | 11 | 32 | 3.82x | 1.31x |
| 38 | Palindrome (number) | Intermediate | 22 | 59 | 20 | 43 | 2.95x | 1.37x |
| 39 | LCM | Intermediate | 30 | 81 | 22 | 48 | 3.68x | 1.69x |
| 40 | HTTP Hello Server | Advanced | 35 | 53 | 58 | 50 | 0.91x | 1.06x |
| 41 | JSON Parse + Sum | Advanced | 29 | 66 | 20 | 65 | 3.30x | 1.02x |
| 42 | Struct + Method | Advanced | 45 | 59 | 61 | 56 | 0.97x | 1.05x |

## Methodology

- **Toke-16K (v0.3):** 16,384-token vocabulary trained on 25,953 v0.3 corpus records plus a set of loke production `.tk` files whose size was published as 698 files but is not reproducible from any artefact in this workspace (withdrawn 2026-09-19, story 132.14). Trained on toke text, so it may only ever be applied to the **toke** column -- never to the Python or Go source.
- **cl100k_base:** OpenAI's 100,277-token vocabulary (used by GPT-4, Claude via tiktoken). The only tokenizer in this table that is valid on all three languages, and therefore the only basis for a cross-language ratio here.
- All toke examples are single-line v0.3 source (optimal for BPE), not canonical `tkc --min` v0.4 text. Multi-line adds ~50% more tokens due to whitespace.
- Python and Go examples are idiomatic (not golfed), representing what a developer would actually write.
- N = 42 hand-picked examples, skewed to short tasks; no confidence intervals were computed. The benchmarked set is the 60 Gate-1 task ids (`toke-eval/docs/gate1-60-v04.md`).

## Where the Toke-16K tokenizer merges most

These are the toke constructs the v0.3 Toke-16K vocabulary collapses into few tokens --
i.e. where the `toke (Toke-16K)` column falls furthest below the `toke (cl100k)` column.
It is a statement about that tokenizer on toke text only, not about toke versus any other
language:
- Multiple function signatures (`:i64):i64{` merges to few tokens)
- Standard library imports (`i=j:std.json` = 1 token)
- Struct types and construction (`$point{x:1;y:2}` = compact)
- HTTP/JSON patterns (very common in the training corpus, heavily merged)

Under cl100k_base -- the tokenizer the models generating this code actually use -- toke is
the *more* expensive of the three languages in this table (1.80x Python, 1.35x Go).

## Where Python wins

Python is more compact when:
- The task is a one-liner with built-in functions (`sum()`, `len()`, `range()`, list comprehensions)
- No imports/module boilerplate needed
- String operations that Python handles natively but toke requires explicit loops

## Interactive Token Visualisation

See the [interactive token visualiser](/tokens.html) to see exactly how each token boundary falls, with colour-highlighted tokens for side-by-side comparison.