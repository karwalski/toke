# std.infer -- In-Process LLM Inference

## Overview

The `std.infer` module provides in-process large language model inference via llama.cpp. Models are loaded from GGUF files (by convention stored in `~/.loke/models/`) and accessed through an opaque `model_handle`. The module supports text generation, embedding extraction, and streaming disk-based model loading for hardware-constrained environments.

The real implementation requires llama.cpp compiled in (`TK_HAVE_LLAMACPP`). Without it, all functions return an `infererr` with a clear "not available" message, so programs compile and fail gracefully at runtime.

## Types

### model_handle

An opaque handle referencing a loaded model. Obtain one from `infer.load` or `infer.load_streaming`. Pass to all other functions. Release with `infer.unload`.

| Field | Type | Meaning |
|-------|------|---------|
| id | Str | Unique identifier for the loaded model instance |

### infer_opts

Options controlling how a model is loaded into memory.

| Field | Type | Meaning |
|-------|------|---------|
| n_gpu_layers | i32 | Number of transformer layers to offload to GPU (0 = CPU only) |
| n_threads | i32 | Number of CPU threads used for inference (0 = use llama.cpp default) |
| seed | i32 | RNG seed for reproducible sampling (-1 = random) |

### stream_opts

Options for disk-streaming model loading (Epic 72.7). Used with `infer.load_streaming`.

| Field | Type | Meaning |
|-------|------|---------|
| ram_ceiling_gb | f32 | Maximum RAM the streaming loader may use (gigabytes) |
| prefetch_layers | i32 | Number of layers to prefetch ahead of the active decode window |
| requires_nvme | bool | If true, refuse to stream from spinning-disk paths |

### infererr

| Field | Type | Meaning |
|-------|------|---------|
| msg  | Str  | Human-readable error description |
| code | i32  | Internal error code (0 if not applicable) |

## Functions

### infer.load(model_path: Str; opts: infer_opts): model_handle!infererr

Loads a GGUF model file from `model_path` into memory. By convention, models live in `~/.loke/models/`. Returns a `model_handle` on success or `infererr` on failure (path not found, out of memory, llama.cpp unavailable, etc.).

`infer_opts` fields default to safe values when zero: GPU layers default to 0 (CPU-only), threads default to the llama.cpp heuristic, seed defaults to a random value.

**Example:**
```toke
let opts = $infer_opts{n_gpu_layers: 32; n_threads: 8; seed: -1};
let h = infer.load("~/.loke/models/mistral-7b.Q4_K_M.gguf"; opts)?;
```

### infer.unload(h: model_handle): bool

Frees all resources associated with `h`. Returns `true` on success, `false` if the handle was already unloaded or invalid. After calling `infer.unload`, the handle must not be used again.

**Example:**
```toke
let ok = infer.unload(h);
```

### infer.generate(h: model_handle; prompt: Str; max_tokens: i32): Str!infererr

Runs autoregressive text generation starting from `prompt`, stopping after at most `max_tokens` new tokens. Returns the generated text only (not including the prompt). Generate and embed calls on the same handle are serialised internally.

**Example:**
```toke
let reply = infer.generate(h; "The capital of France is"; 64)?;
log.info(reply);  (* " Paris." *)
```

### infer.embed(h: model_handle; text: Str): @(f32)!infererr

Encodes `text` through the model's embedding layer and returns a float array of the embedding vector. The array is heap-allocated; **the caller is responsible for freeing it** when done.

**Example:**
```toke
let vec = infer.embed(h; "hello world")?;
(* use vec, then free when done *)
```

### infer.is_loaded(h: model_handle): bool

Returns `true` if `h` refers to an active, loaded model; `false` if it has been unloaded or was never successfully loaded.

**Example:**
```toke
if infer.is_loaded(h) {
    let reply = infer.generate(h; prompt; 256)?;
}
```

### infer.load_streaming(model_dir: Str; opts: stream_opts): model_handle!infererr

**(Epic 72.7 — not yet implemented.)** Loads a model from `model_dir` using disk-streaming so that only the active layer window needs to reside in RAM. This allows running models larger than available RAM at the cost of latency. Returns `infererr` with code -2 in the current release.

**Example:**
```toke
let opts = $stream_opts{ram_ceiling_gb: 8.0; prefetch_layers: 4; requires_nvme: true};
let h = infer.load_streaming("~/.loke/models/llama-70b-Q4/"; opts)?;
```

## Notes

- Model files must be in GGUF format. The llama.cpp project provides quantised GGUF downloads.
- `infer.embed` returns a raw `@(f32)` whose length equals the model's embedding dimension. Check `infererr` before accessing the array.
- Thread safety: concurrent `infer.generate` and `infer.embed` calls on the **same** handle are serialised via an internal per-handle mutex. Calls on **different** handles may proceed concurrently.
- When `TK_HAVE_LLAMACPP` is not defined at compile time, all functions return `infererr{msg: "std.infer: llama.cpp not compiled in"; code: -1}`.
