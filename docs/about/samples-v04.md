# v0.4 code samples — toke vs Python, every tokenizer lane

**Story 132.0(b).** The public site's side-by-side samples (`toke-website/templates/index.tkt`,
`tokens.tkt`, `tokenizer.tkt`) were cut under **v0.3** syntax and quote a single
unlabelled headline ("a complete fibonacci program uses 24 tokens — compared to 41 in
Python … a 42% reduction vs Python"). That number compares **the toke BPE on toke** with
**cl100k on Python** — two different tokenizers — and cannot be quoted. This file is the
v0.4 replacement dataset. Machine-readable copy: `samples-v04.json`.

Regenerate with:

```
python3 scripts/about/samples_v04.py     # writes samples-v04.json and the block below
```

## What was measured

- **Sources:** `docs/about/samples-v04/<name>.{tk,py}` — four task pairs.
- **Verification:** every pair is **execution-verified** — the toke binary and the Python
  program are run and their stdout must be byte-identical before any number is emitted.
  All four toke programs are `tkc --check` clean and `tkc --lint` clean under **toke 2.8.0**.
- **toke basis:** the `tkc --min` canonical form (116/B2: measuring readable source
  understated toke by 28.2%; `--min` is the form a model emits).
- **Python basis:** the source as written — PEP 8, type hints, no comments or docstrings.
  Python has no canonical minimal form, so there is no `--min` equivalent; this asymmetry
  is stated rather than hidden.
- **Lanes** (per story 131.50 — a published claim names its tokenizer lane or it does not
  ship): raw UTF-8 bytes, `proxy8k` (the toke byte-level BPE proxy,
  `patterns/proxy/proxy8k-*.json`), `tokenizer_v03` (the v0.3 HF tokenizer),
  `Qwen2.5-Coder-7B`, `cl100k_base`, `o200k_base`.

## Results

Every number below is labelled with the tokenizer that produced it. Counts are of the
unmasked text; TEMSpec string-body-masked counts for the toke side are in the JSON
(`tokens_masked`).

<!-- samples-v04:begin -->
| sample | side | bytes | proxy8k | tokenizer_v03 | qwen25coder | cl100k | o200k |
|---|---|---:|---:|---:|---:|---:|---:|
| fib | toke v0.4 (`--min`) | 111 | 23 | 26 | 63 | 59 | 60 |
| fib | Python 3 | 140 | 99 | 92 | 51 | 50 | 50 |
| fizzbuzz | toke v0.4 (`--min`) | 193 | 55 | 59 | 89 | 86 | 87 |
| fizzbuzz | Python 3 | 247 | 189 | 193 | 74 | 72 | 72 |
| sumeven | toke v0.4 (`--min`) | 168 | 37 | 43 | 95 | 93 | 96 |
| sumeven | Python 3 | 168 | 132 | 118 | 75 | 74 | 74 |
| vowels | toke v0.4 (`--min`) | 221 | 63 | 75 | 108 | 106 | 106 |
| vowels | Python 3 | 198 | 147 | 135 | 68 | 68 | 68 |
| **total (4)** | **toke v0.4 (`--min`)** | **693** | **178** | **203** | **355** | **344** | **349** |
| **total (4)** | **Python 3** | **753** | **567** | **538** | **268** | **264** | **264** |
| **toke / Python** | **density ratio** | 0.92× | 0.31× | 0.38× | 1.32× | 1.30× | 1.32× |
<!-- samples-v04:end -->

## How to read this table — and how not to

1. **The toke-vs-Python columns are a cross-language density ratio (TEMSpec §2.3),
   informational only.** They are *not* a same-tokenizer token reduction and must never be
   quoted as one. A same-tokenizer reduction compares two texts under one tokenizer; this
   compares two languages.
2. **Under a general-purpose tokenizer, toke costs more than Python on these samples.**
   cl100k: 344 vs 264 → **toke / Python = 1.30×**. o200k 1.32×, Qwen2.5-Coder 1.32×. This
   matches the independent KERN finding (≈1.76× on the Gate-1 60 before the v0.4 rewrite)
   and story 133.4's post-rewrite **1.34×** on those 60 programs. The site's "42% reduction
   vs Python" is the opposite of what a same-tokenizer measurement shows, and story **132.6**
   tracks removing it everywhere.
3. **The `proxy8k` and `tokenizer_v03` columns cannot be used for the Python side at all.**
   Both were trained on toke text; applied to Python they are out-of-domain and inflate the
   Python count mechanically (proxy8k: 567 for Python vs 178 for toke). Those two columns
   are meaningful only *within* the toke side — e.g. toke under proxy8k (178) vs toke under
   cl100k (344). Quoting proxy8k-on-toke against cl100k-on-Python is exactly the error the
   current site copy makes.
4. **`tokenizer_v03` is lossy** — its null `unk_token` silently drops backslashes
   (`docs/metrics-baseline.md`, 131.20). It is listed for continuity with the v0.3 site
   figures, not as a basis for any claim.
5. **Bytes are the one lane with no tokenizer assumption:** toke 693 vs Python 753
   (**0.92×**) across these four samples. It is also the narrowest margin, which is the
   honest shape of the result at this sample size.
6. **N = 4.** These are illustrative samples, not a benchmark. The benchmarked set is the
   60 Gate-1 task ids in `toke-eval/docs/gate1-60-v04.md` (see
   `toke-eval-drift-decision.md` for what that set may and may not claim).

## Samples

| id | task | verified stdout |
|---|---|---|
| `fib` | 10th Fibonacci number, recursive | `55` |
| `fizzbuzz` | FizzBuzz, 1..15 | 15 lines |
| `sumeven` | Sum of the squares of the even numbers in a list | `220` |
| `vowels` | Count the vowels in a string | `5` |

The v0.4 fibonacci that replaces the site's v0.3 cut:

```
m=fib;i=io:std.io;f=fib(n:i64):i64{if(n<2){<n;}<fib(n-1)+fib(n-2);};f=main():i64{io.println("\(fib(10))");<0;};
```

(The old site sample omits the `std.io` import and prints via a bare `io.println(fib(10))`,
which does not type-check under 2.8.0 — `io.println` takes a `str`, so the interpolation is
required. `tokens.tkt` additionally computes `fib(35)` while the Python side computes
`fib(10)`; the pairs here compute the same value on both sides and are output-verified.)
