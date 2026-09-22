# std.mlx -- MLX Inference Backend

## Overview

The `std.mlx` module provides an optional MLX inference backend for Apple Silicon Macs. It communicates with a local MLX Python bridge server (default port 11438) over plain HTTP/1.1. On non-Apple platforms, or when the bridge is not running, `mlx.is_available()` returns `false` and all other functions return an error.

The bridge server is a separate process — see the toke MLX bridge documentation for how to start it. This module does not embed or link the MLX framework directly; it speaks a simple REST protocol to the bridge.

## Types

### mlx_model

An opaque handle representing a loaded model.

| Field | Type | Meaning |
|-------|------|---------|
| id    | str  | Unique handle identifier returned by the bridge |
| path  | str  | File-system path from which the model was loaded |

### mlxerr

Returned on failure from fallible functions.

| Field | Type | Meaning |
|-------|------|---------|
| msg   | str  | Human-readable description of the error |

## Functions

### mlx.is_available(): bool

Returns `true` when both conditions are satisfied:

1. The process is running on Apple Silicon (detected via `hw.optional.arm64` sysctl).
2. The MLX bridge is reachable — a GET to `http://localhost:11438/health` returns HTTP 200.

Returns `false` on any non-Apple platform, when not running on ARM64, or when the bridge is not responding.

**Example:**
```toke
if mlx.is_available() {
    let m = mlx.load("/models/mistral-7b");
}
```

### mlx.load(model_path: str): $mlx_model!mlxerr

Asks the bridge to load a model from `model_path`. On success returns an `mlx_model` handle.

POSTs `{"model_path": "<model_path>"}` to `http://localhost:11438/load`.

**Example:**
```toke
let m = mlx.load("/models/mistral-7b") else |e| die(e.msg);
```

### mlx.unload(m: $mlx_model): bool

Asks the bridge to release the model associated with handle `m`. Returns `true` on success, `false` if the bridge returned an error or was unreachable.

POSTs `{"id": "<m.id>"}` to `http://localhost:11438/unload`.

**Example:**
```toke
let ok = mlx.unload(m);
```

### mlx.generate(m: $mlx_model, prompt: str, max_tokens: i32): str!mlxerr

Generates text from `prompt` using model `m`, stopping after at most `max_tokens` tokens. Returns the generated string on success.

POSTs `{"id": "<m.id>", "prompt": "<prompt>", "max_tokens": <max_tokens>}` to `http://localhost:11438/generate`.

**Example:**
```toke
let reply = mlx.generate(m; "Explain quantum entanglement."; 256) else |e| die(e.msg);
```

### mlx.embed(m: $mlx_model, text: str): @(f32)!mlxerr

Computes a dense embedding vector for `text` using model `m`. Returns an array of `f32` values. The dimensionality depends on the model.

POSTs `{"id": "<m.id>", "text": "<text>"}` to `http://localhost:11438/embed`.

**Example:**
```toke
let vec = mlx.embed(m; "Hello world") else |e| die(e.msg);
```

## Bridge protocol

All requests are JSON over HTTP/1.1 to `localhost:11438`. Successful responses carry HTTP 200 and a JSON body. Error responses carry HTTP 4xx/5xx with `{"error": "<message>"}`.

| Endpoint   | Method | Request body                                         | Success response                     |
|------------|--------|------------------------------------------------------|--------------------------------------|
| /health    | GET    | (none)                                               | `{"status":"ok"}`                    |
| /load      | POST   | `{"model_path":"..."}`                               | `{"id":"...","path":"..."}`           |
| /unload    | POST   | `{"id":"..."}`                                       | `{"ok":true}`                        |
| /generate  | POST   | `{"id":"...","prompt":"...","max_tokens":N}`         | `{"text":"..."}`                      |
| /embed     | POST   | `{"id":"...","text":"..."}`                          | `{"embedding":[f32,...]}`             |

## Platform notes

- The module compiles on all platforms. On non-Apple targets, every function is a no-op stub (is_available returns false; load/generate/embed return errors).
- On Apple Silicon, the sysctl check runs once per process invocation. The bridge reachability check runs on every call to `mlx.is_available()`.
- The default port 11438 is fixed at compile time. To change it, recompile with `-DMLX_BRIDGE_PORT=<port>`.
