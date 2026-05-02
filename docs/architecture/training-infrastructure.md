# Training Infrastructure — 1B Purpose-Built Model

Story 81.3

## 1. Hardware Options

### Primary: Mac Studio M4 Max (128 GB unified memory)

- **Framework:** MLX (Apple Silicon native, Metal backend)
- **Estimated training time** for 1B model: ~7-14 days continuous
- **Memory fit:** 1B params x 4 bytes = 4 GB; with gradients and optimizer state ~16 GB — fits comfortably in 128 GB unified memory
- **Advantages:** no network dependency, no cloud costs

### Alternative: Cloud GPU (for speed)

- AWS p4d.24xlarge (8x A100) or equivalent
- Estimated training time: ~12-24 hours
- Cost: ~$200-500 per full training run
- Use for hyperparameter search and final production run

## 2. Framework: MLX

- Native Apple Silicon support (Metal backend)
- Lazy evaluation, unified memory model
- Clean Python API similar to JAX/PyTorch
- Already used in toke-model repo for fine-tuning (`train_mlx.py` exists)
- For cloud runs: PyTorch with DeepSpeed as fallback

## 3. Training Pipeline

| Stage | Detail |
|---|---|
| Data loading | Streaming JSONL from corpus, tokenized on-the-fly |
| Tokenizer | Purpose-built BPE (trained with `tokenizers` library) |
| Model | Custom decoder-only transformer (see 81.1 architecture) |
| Optimizer | AdamW with cosine learning rate schedule |
| Checkpointing | Every 1000 steps to disk |
| Monitoring | Loss curves, gradient norms, learning rate (logged to JSONL) |
| Evaluation | Run Pass@1 benchmark every 5000 steps |

## 4. Training Configuration

```
model_size:          1B
context_length:      2048
batch_size:          32 (micro-batch 4, gradient accumulation 8)
learning_rate:       3e-4 (peak), cosine decay to 3e-5
warmup_steps:        500
total_steps:         50000-100000 (depends on corpus size)
weight_decay:        0.1
gradient_clipping:   1.0
precision:           bfloat16 (mixed precision)
```

## 5. Checkpointing and Recovery

- Save model weights, optimizer state, and training step every 1000 steps
- Keep last 5 checkpoints (auto-delete older)
- Resume from any checkpoint on crash/restart
- Export best checkpoint to MLX format for inference

## 6. Evaluation During Training

- Every 5000 steps: run 100-task subset of benchmark
- Track: Pass@1, compilation rate, illegal character rate
- Early stopping: if Pass@1 plateaus for 3 consecutive evaluations
- Final evaluation: full 1000-task benchmark on best checkpoint

## 7. Cost Estimate

| Option | Cost | Notes |
|---|---|---|
| Local (M4 Max) | ~$5-10 | Electricity only, ~14 days |
| Cloud (single A100) | ~$50-100/run | 3-5 runs = $150-500 |
| Cloud (8x A100) | ~$200-500/run | Faster iteration |

**Recommendation:** develop and debug locally, production run on cloud.

## 8. Repository Structure (in toke-model/)

```
toke-model/
  train/
    train_1b.py          # Main training script
    model.py             # 1B architecture definition
    data.py              # Corpus loader and tokenizer
    config.py            # Training hyperparameters
    eval_during_train.py # Periodic evaluation
  tokenizer/
    train_bpe.py         # BPE tokenizer training
    vocab/               # Trained tokenizer files
  checkpoints/           # Model checkpoints
  logs/                  # Training logs (JSONL)
```
