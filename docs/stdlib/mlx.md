---
title: std.mlx
slug: mlx
section: reference/stdlib
order: 48
---

> **Status: FAÇADE -- do not write against this page's API.** `src/stdlib/mlx.c` is 647 lines of complete, working bridge client. Almost none of it is reachable from toke, because `src/stdlib/mlx_glue.c` is 34 lines of stubs. Every published function links at its published arity (since 137.11), and every one is a stub that reports failure: `isavailable` and `unload` return `false`, the other three take the `$err` arm. Nothing on this page has ever reached the bridge from a toke program. This is the same class as story 136.44 (`std.tls`) and 136.16 (`std.toon`): a finished C core behind glue that returns 0.

`std.mlx` is intended to be an MLX inference backend for Apple Silicon, talking to a local MLX Python bridge server on port 11438 over plain HTTP/1.1. It does not embed or link the MLX framework; it speaks a small REST protocol to a separate bridge process.

## What is actually reachable today

Measured against the built compiler, not read off the interface.

| Published call | `.tki` arity | Glue arity | Reachable | What happens |
|---|---|---|---|---|
| `mlx.isavailable` | 0 | 0 | links | `false` on **every** platform, Apple Silicon included -- deliberately (137.11): no other wrapper reaches the core, so reporting the bridge as available would promise calls that cannot succeed. The sysctl and health probe in `mlx.c` are never called |
| `mlx.load` | 1 | 1 | links | always takes the `$err` arm with an `mlxerr` saying MLX is not available; `mlx_load` in the core is never called |
| `mlx.generate` | 3 | 3 | links | takes `maxtokens` (137.11; the wrapper had no slot for it) and ignores it; always takes the `$err` arm (until 127.241 it returned the literal string `"[mlx: not available]"` through `$ok`) |
| `mlx.unload` | 1 | 1 | links | returns `false` -- `load` never hands out a model, so nothing is unloaded (137.11 added the wrapper; before it the call failed at link) |
| `mlx.embed` | 2 | 2 | links | always takes the `$err` arm with an `mlxerr` saying MLX is not available (127.241 added the wrapper; before it the call failed to link) |

Until 137.11 a program that called `mlx.generate(h; prompt; 256)` as the interface publishes it was rejected with `E4026 wrong number of arguments` (the wrapper took two), and `mlx.unload` failed at link.

Because `isavailable` always returns `false`, the only correct thing a toke program can do with this module today is notice that and take the other branch. That is what the one compiling example below does, and it is deliberately the whole example.

```toke
m=mlxprobe;
i=mlx:std.mlx;
i=io:std.io;

f=main():i64{
  (* isavailable is false in the glue until the glue reaches the core. This
     branch is unreachable until then; the else branch is what actually runs. *)
  if(mlx.isavailable()){
    io.println("MLX bridge reachable")
  }el{
    io.println("no MLX backend: use std.llm against a remote provider")
  };
  <0
};
```

## The naming problem, now fixed

Toke's default 59-character profile excludes `_`, so the published type `mlx_model` could not be written in a toke program at all -- `$mlx_model{...}` would not lex. Story 136.46 renamed it to `mlxmodel`, and `make check-tki-names` now rejects any interface identifier the profile cannot express. There were no call sites to migrate, because there could not be any.

The prose defect this page carried independently was `model_path` and `max_tokens` as parameter names throughout; those are spelled `modelpath` and `maxtokens` below. A restored module should also keep handles opaque as `i64`, the way `std.tls` does since 136.44, so no field is ever named at a call site.

## The design, kept as the record

Everything below describes what the C core in `src/stdlib/mlx.c` already implements and what a restored module should expose. **It is not callable.**

### Types

#### mlxmodel

An opaque handle to a loaded model. The core carries `id` (the bridge's handle identifier) and `path` (where the model was loaded from). Spelled `mlx_model` before 136.46; a restoration should keep it opaque as an `i64`.

#### mlxerr

`msg: str` -- a human-readable description of the failure.

### Functions

#### mlx.isavailable(): bool

Intended to be `true` only when both hold:

1. the process is on Apple Silicon (`hw.optional.arm64` sysctl), and
2. the bridge answers -- `GET http://localhost:11438/health` returns 200.

`mlx_is_available` in the core does exactly this. The glue does not call it, and returns `false` on purpose while every other wrapper is a stub.

#### mlx.load(modelpath: str): mlxmodel!mlxerr

POSTs `{"model_path": "..."}` to `/load` and returns a handle. `mlx_load` in the core does this; the glue does not call it, and every call takes the `$err` arm with an `mlxerr` whose `msg` says so.

#### mlx.unload(m: mlxmodel): bool

POSTs `{"id": "..."}` to `/unload`. `mlx_unload` exists in the core. The wrapper (137.11) does not call it and returns `false`.

#### mlx.generate(m: mlxmodel; prompt: str; maxtokens: i32): str!mlxerr

POSTs `{"id": ..., "prompt": ..., "max_tokens": N}` to `/generate`. `mlx_generate` in the core takes the token limit. The wrapper takes all three arguments (137.11) and ignores them; every call takes the `$err` arm.

#### mlx.embed(m: mlxmodel; text: str): @(f32)!mlxerr

POSTs `{"id": ..., "text": ...}` to `/embed`. `mlx_embed` exists in the core, complete with the JSON float-array parser. The wrapper (127.241) is a stub like the rest: it does not call the core, and every call takes the `$err` arm.

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

The cost is a real `mlx_glue.c`: the five wrappers, which exist at the published arities (127.241, 137.11), calling the five core functions, and `isavailable` calling `mlx_is_available`. The type rename is already done (136.46). No new C is needed -- the core is done. The work is the layer that was never written.

## See Also

- `std.infer` -- the sibling local-inference module, in exactly the same state: complete core, stub glue.
- `std.llm` -- reaching a remote provider, and the only working option today.
