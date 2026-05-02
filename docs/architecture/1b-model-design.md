# 1B Parameter Model Architecture for Toke

**Story:** 81.1 — Design the 1B parameter model architecture
**Status:** Draft
**Date:** 2026-05-01

---

## 1. Architecture Choice: Decoder-Only Transformer

Code generation is autoregressive — the model predicts the next token given all
previous tokens. A decoder-only transformer is the standard architecture for this
task (GPT, CodeGen, StarCoder all use it). No encoder is needed because the input
is a prompt and the output is toke code; there is no separate "source language" to
encode.

## 2. Parameter Budget (1B)

| Component              | Value                |
|------------------------|----------------------|
| Embedding dimension    | 2048                 |
| Layers                 | 24                   |
| Attention heads        | 16                   |
| KV heads (GQA)         | 4                    |
| Feed-forward dimension | 8192 (4x hidden)     |
| Context length         | 2048 tokens          |
| Vocabulary size        | 8K–16K (BPE)        |

Context length rationale: toke programs are short. The 55-character alphabet and
compact syntax mean 2K tokens covers very long programs comfortably.

Vocabulary size rationale: a purpose-built BPE tokenizer trained exclusively on
toke corpus. 8K–16K is sufficient because the character and keyword set is small.

## 3. Key Design Decisions

### Rotary Position Embeddings (RoPE)

Standard for modern transformer models. Handles variable sequence lengths
naturally and does not require learning absolute position embeddings.

### Grouped Query Attention (GQA)

4 KV heads shared across 16 query heads (4:1 ratio). This is memory-efficient
for inference on Apple Silicon — reduces KV cache size by 4x compared to
multi-head attention while maintaining quality.

### SwiGLU Activation

Replaces ReLU in the feed-forward blocks. SwiGLU produces better results on
code generation tasks and is used by LLaMA, Mistral, and other modern
architectures.

### Pre-norm (RMSNorm)

RMSNorm applied before each sub-layer rather than after. This produces more
stable training gradients than post-norm LayerNorm, especially important when
training from scratch on a small corpus.

### No Dropout

Deliberately omitted. The toke corpus is small and the syntax surface is
limited. We want the model to fully memorise toke's patterns — grammar rules,
stdlib API surface, and idiomatic constructs. Dropout would fight this goal.

## 4. Why 1B Is Enough

Toke's language surface is vastly smaller than general-purpose languages:

- **55 characters** (no uppercase, no underscore in default syntax)
- **13 keywords**
- **LL(1) grammar** — deterministic, no ambiguity
- **No competing knowledge** — the model learns only toke, not Python/C/JS

A 1B model can fully memorise the grammar and stdlib API surface. A 7B model is
overkill — most parameters would encode unused capacity for patterns that do not
exist in toke.

Practical benefits of 1B over 7B:

- Inference is ~7x faster
- Fits comfortably on consumer hardware (M4 Mac, ~2GB memory)
- Training is feasible on a single machine
- Smaller model = faster iteration on architecture and training experiments

## 5. Training Plan

### Tokenizer

Purpose-built BPE tokenizer trained on the toke corpus. Target vocabulary size
8K. Every token the model learns is a toke token — no wasted vocabulary entries
for Python, JavaScript, or natural language.

### Data

Comment-free toke programs in 55-character default syntax (mt match, no
underscore). Programs are stripped of comments to focus the model on code
structure rather than natural language.

### Curriculum

Train simple to complex:

1. Single expressions and statements
2. Single functions
3. Multi-function modules
4. Multi-module programs with imports and stdlib usage

### Loss

Standard cross-entropy on next-token prediction. No auxiliary losses needed.

### Hardware

- Primary: Mac Studio M4 Max (local, MLX-native)
- Fallback: cloud GPU for speed if local training is too slow

### Framework

- **MLX** (preferred): Apple Silicon native, no CPU-GPU transfer overhead
- **PyTorch**: fallback if MLX lacks needed features

## 6. Evaluation Targets

| Metric                | Target               | Baseline (fine-tuned 7B) |
|-----------------------|----------------------|--------------------------|
| Pass@1 (held-out)     | > 80%               | 63.7%                    |
| Illegal character rate| < 0.1%              | ~2.5%                    |
| Token efficiency      | within 10% of ref   | not measured              |
| Inference speed (M4)  | > 100 tok/s         | ~15 tok/s                |

**Pass@1 > 80%** is the primary gate. The model must generate correct toke
programs on the first attempt more than 80% of the time on a held-out benchmark.

**Illegal character rate < 0.1%** ensures the model has internalised toke's
55-character alphabet. The fine-tuned 7B achieved ~2.5% — a purpose-built model
trained only on legal toke should do far better.

**Token efficiency within 10%** means generated programs are close in length to
hand-written reference solutions — no padding, no unnecessary verbosity.

**Inference speed > 100 tok/s** on M4 Max ensures the model is practical for
interactive use (IDE completion, REPL assistance).

## 7. Comparison with Fine-Tuning Approach

| Metric           | Fine-tuned 7B (Gate 1) | Purpose-built 1B (target) |
|------------------|------------------------|---------------------------|
| Parameters       | 7B                     | 1B                        |
| Training data    | toke + Python/C/etc contamination | toke only        |
| Pass@1           | 63.7%                  | > 80%                     |
| Illegal chars    | ~2.5%                  | < 0.1%                    |
| Inference speed  | ~15 tok/s (M4)         | > 100 tok/s (M4)          |
| Memory           | ~14 GB                 | ~2 GB                     |

The fine-tuned 7B model carries knowledge of dozens of programming languages.
Most of its 7 billion parameters encode patterns irrelevant to toke. The
purpose-built 1B dedicates every parameter to toke, producing a smaller, faster,
and more accurate model.
