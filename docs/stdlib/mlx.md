---
title: std.mlx
slug: mlx
section: reference/stdlib
order: 48
---

> **Status: FAÇADE -- do not write against this page's API.** `src/stdlib/mlx.c` is 647 lines of complete, working bridge client. Almost none of it is reachable from toke, because `src/stdlib/mlx_glue.c` is 34 lines of stubs. Two of the five published functions have **no `_w` wrapper at all** and fail at link; a third has the wrong arity; the two that do link are hard-coded to report failure. Nothing on this page has ever run from a toke program. This is the same class as story 136.44 (`std.tls`) and 136.16 (`std.toon`): a finished C core behind glue that returns 0.

`std.mlx` is intended to be an MLX inference backend for Apple Silicon, talking to a local MLX Python bridge server on port 11438 over plain HTTP/1.1. It does not embed or link the MLX framework; it speaks a small REST protocol to a separate bridge process.

## What is actually reachable today

Measured against the built compiler, not read off the interface.

| Published call | `.tki` arity | Glue arity | Reachable | What happens |
|---|---|---|---|---|
| `mlx.isavailable` | 0 | 0 | links | hard-coded `return 0` on **every** platform, Apple Silicon included -- the sysctl and health probe in `mlx.c` are never called |
| `mlx.load` | 1 | 1 | links | hard-coded `return 0`; `mlx_load` in the core is never called |
| `mlx.generate` | 3 | **2** | links at 2 args | drops `maxtokens` entirely and returns the literal string `"[mlx: not available]"` |
| `mlx.unload` | 1 | — | **no** | `E9003`, undefined symbol `_tk_mlx_unload_w` |
| `mlx.embed` | 2 | — | **no** | `E9003`, undefined symbol `_tk_mlx_embed_w` |

A program that calls `mlx.generate(h; prompt; 256)` as this page used to document it is rejected before it reaches the linker:

```
E4026 wrong number of arguments for 'std.mlx.generate':
      the implementation takes 2 arguments, the call passes 3
```

Because `isavailable` always returns `false`, the only correct thing a toke program can do with this module today is notice that and take the other branch. That is what the one compiling example below does, and it is deliberately the whole example.

```toke
m=mlxprobe;
i=mlx:std.mlx;
i=io:std.io;

f=main():i64{
  (* isavailable is hard-coded false in the glue. This branch is unreachable
     until the glue is written; the else branch is what actually runs. *)
  if(mlx.isavailable()){
    io.println("MLX bridge reachable")
  }el{
    io.println("no MLX backend: use std.llm against a remote provider")
  };
  <0
};
```

## The underscore problem, on top of the glue problem

Even with working glue, two names on the published interface cannot be written in a toke program at all: toke's default 59-character profile excludes `_`.

- the type `mlx_model` -- so `$mlx_model{...}` cannot be lexed;
- `model_path` and `max_tokens`, used as parameter names throughout the original page.

Restoring the module therefore needs the same treatment `std.tls` got in 136.44: rename the type (`mlxmodel`), and keep handles opaque as `i64` so no field is ever named.

## The design, kept as the record

Everything below describes what the C core in `src/stdlib/mlx.c` already implements and what a restored module should expose. **It is not callable.**

### Types

#### mlxmodel

An opaque handle to a loaded model. The core carries `id` (the bridge's handle identifier) and `path` (where the model was loaded from). Published today as `mlx_model`, which is unspellable; a restoration should rename it and keep it opaque as an `i64`.

#### mlxerr

`msg: str` -- a human-readable description of the failure.

### Functions

#### mlx.isavailable(): bool

Intended to be `true` only when both hold:

1. the process is on Apple Silicon (`hw.optional.arm64` sysctl), and
2. the bridge answers -- `GET http://localhost:11438/health` returns 200.

`mlx_is_available` in the core does exactly this. The glue does not call it.

#### mlx.load(modelpath: str): mlxmodel!mlxerr

POSTs `{"model_path": "..."}` to `/load` and returns a handle. `mlx_load` in the core does this; the glue returns 0.

#### mlx.unload(m: mlxmodel): bool

POSTs `{"id": "..."}` to `/unload`. `mlx_unload` exists in the core. **No wrapper exists**, so a call fails at link.

#### mlx.generate(m: mlxmodel; prompt: str; maxtokens: i32): str!mlxerr

POSTs `{"id": ..., "prompt": ..., "max_tokens": N}` to `/generate`. `mlx_generate` in the core takes the token limit. The wrapper takes two arguments and ignores it, so even a two-argument call that compiles silently loses the bound.

#### mlx.embed(m: mlxmodel; text: str): @(f32)!mlxerr

POSTs `{"id": ..., "text": ...}` to `/embed`. `mlx_embed` exists in the core, complete with the JSON float-array parser. **No wrapper exists**, so a call fails at link.

### Bridge protocol

The core implements all five endpoints against `localhost:11438`. Success is HTTP 200 with a JSON body; failure is 4xx/5xx with `{"error": "..."}`.

| Endpoint | Method | Request | Success |
|---|---|---|---|
| `/health` | GET | (none) | `{"status":"ok"}` |
| `/load` | POST | `{"model_path":"..."}` | `{"id":"...","path":"..."}` |
| `/unload` | POST | `{"id":"..."}` | `{"ok":true}` |
| `/generate` | POST | `{"id":"...","prompt":"...","max_tokens":N}` | `{"text":"..."}` |
| `/embed` | POST | `{"id":"...","text":"..."}` | `{"embedding":[...]}` |

The port is fixed at compile time; changing it means rebuilding with `-DMLX_BRIDGE_PORT=<port>`.

## Restoring it

The cost is a real `mlx_glue.c`: five wrappers calling the five core functions, `_w` symbols for `unload` and `embed` that do not exist yet, `generate` widened to three parameters to match the interface, and the `mlx_model` rename. No new C is needed -- the core is done. The work is the layer that was never written.

## See Also

- `std.infer` -- the sibling local-inference module, in exactly the same state: complete core, stub glue.
- `std.llm` -- reaching a remote provider, and the only working option today.
