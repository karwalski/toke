---
title: std.infer
slug: infer
section: reference/stdlib
order: 45
---

> **Status: FAÇADE -- do not write against this page's API.** All six published functions have a `_w` wrapper, so a call links; every one of those wrappers is a stub that ignores its arguments. `infer.load` always takes the `$err` arm, so no handle is ever produced, so nothing else can do anything useful. `src/stdlib/infer.c` is 397 lines implementing `tk_infer_load`, `tk_infer_unload`, `tk_infer_generate` and `tk_infer_embed`; `src/stdlib/infer_glue.c` calls none of them. Same class as story 136.44 (`std.tls`) and 136.16 (`std.toon`): a finished core behind glue that returns 0.

`std.infer` is intended to provide in-process LLM inference via llama.cpp. Models are GGUF files, at whatever path the caller passes, reached through an opaque handle, with text generation, embedding extraction, and a disk-streaming loader for machines that cannot hold the whole model in RAM.

## What is actually reachable today

Measured against the built compiler, not read off the interface. Every row links. Since 127.241 every fallible call says so: it takes the `$err` arm with an `infererr` whose `msg` says local inference is not linked into the build and whose `code` is 0 -- distinct from the core's own -1 ("llama.cpp not compiled in") and -2 ("not implemented"). Before 127.241 nothing errored and the module simply did nothing.

| Published call | `.tki` arity | Glue arity | Behaviour today |
|---|---|---|---|
| `infer.load` | 2 | 2 | takes `inferopts` (135.31) and ignores it; takes the `$err` arm, never a handle |
| `infer.loadstreaming` | 2 | 2 | takes `streamopts` (135.31) and ignores it; takes the `$err` arm. Incomplete in the core too: it returns no handle in any build (136.78; see `infer.loadstreaming` below) |
| `infer.unload` | 1 | 1 | returns 0 |
| `infer.generate` | 3 | 3 | takes the token limit (137.11) and ignores it; takes the `$err` arm (until 127.241 it returned the literal `"[infer: not available]"` through `$ok`) |
| `infer.embed` | 2 | 2 | takes the `$err` arm (until 127.241 it returned a zero-length array through `$ok`) |
| `infer.isloaded` | 1 | 1 | returns 0. Added by 131.38, which found it declared with no wrapper at all -- and made it a stub consistent with the rest rather than a working function, because `load` never hands out a handle for it to check |

Every call now compiles with the arguments the interface publishes. Until 135.31 (`load`, `loadstreaming`) and 137.11 (`generate`) the wrappers had no slot for the options record or the token limit, and a call written as published was rejected with `E4026 wrong number of arguments`.

The arity drops were not cosmetic. `inferopts` carries the GPU-layer count, the thread count and the RNG seed; `streamopts` carries the RAM ceiling and the prefetch depth; `generate`'s third argument is the token limit. A restored module that had kept the old wrapper signatures would have compiled every caller and honoured none of their configuration -- which is exactly the defect class Epic 136 was opened to close.

Because `isloaded` is the only call whose stub answer is *honest* (nothing is loaded, and that is true), the single compiling example below is a guard, and that is all it can be.

```toke
m=inferprobe;
i=infer:std.infer;
i=io:std.io;

f=main():i64{
  (* load is a stub that returns 0, so this handle is never valid and
     isloaded is correspondingly false. The else branch is what runs. *)
  let o=$inferopts{ngpulayers:0;nthreads:0;seed:-1};
  let loaded=mt infer.load("/models/mistral-7b.Q4KM.gguf";o) {
    $ok:h  infer.isloaded(h);
    $err:e false
  };
  if(loaded){
    io.println("model loaded")
  }el{
    io.println("no local inference: std.infer glue is a stub")
  };
  <0
};
```

## The naming problem, now half fixed

Toke's default 59-character profile excludes `_`, so until story 136.46 most of this module's published type and field names could not be written in a toke program at all. 136.46 renamed them, and `make check-tki-names` now rejects any interface identifier the profile cannot express:

| Was | Is now | Kind |
|---|---|---|
| `model_handle` | `modelhandle` | type |
| `infer_opts` | `inferopts` | type |
| `stream_opts` | `streamopts` | type |
| `n_gpu_layers`, `n_threads` | `ngpulayers`, `nthreads` | field |
| `ram_ceiling_gb`, `prefetch_layers`, `requires_nvme` | `ramceilinggb`, `prefetchlayers`, `requiresnvme` | field |

There were no call sites to migrate, because there could not be any.

What 136.46 does **not** fix is the prose: this page previously documented `infer.load_streaming` and `infer.is_loaded`, which the interface has spelled `loadstreaming` and `isloaded` since 113.2a. That was a documentation defect independent of the glue, and it is corrected below.

Since 135.31 the options records can be built as literals (`$inferopts{ngpulayers:0;nthreads:1;seed:1}`) and passed to `load` / `loadstreaming`, whose wrappers take them -- and ignore them, being stubs. Restoring the module should follow the `std.tls` pattern from 136.44 and add constructor functions (`infer.opts(gpulayers; threads; seed)`) rather than relying on struct literals, keeping handles opaque as `i64`.

## The design, kept as the record

Everything below describes what `src/stdlib/infer.c` already implements and what a restored module should expose. **It is not callable as written.**

### Types

#### modelhandle

An opaque handle to a loaded model, carrying the model instance id. It has no readable fields: reading `h.id` is a compile error, E4034 (138.65); `infer.isloaded` says whether it names a loaded model. Spelled `model_handle` before 136.46.

#### inferopts

How to load: `ngpulayers` to offload (0 = CPU only), `nthreads` (0 = the llama.cpp heuristic), `seed` (-1 = random). Spelled `infer_opts` before 136.46.

#### streamopts

Disk-streaming load (Epic 72.7): `ramceilinggb`, `prefetchlayers` ahead of the decode window, and `requiresnvme` to refuse a spinning disk. Spelled `stream_opts` before 136.46.

#### infererr

`msg: str` and `code: i32`.

### Functions

#### infer.load(modelpath: str; opts: inferopts): modelhandle!infererr

Loads a GGUF file into memory. `tk_infer_load` in the core does this and takes the options; the wrapper takes the path and the options (135.31) and always takes the `$err` arm.

#### infer.unload(h: modelhandle): bool

Frees the model. `tk_infer_unload` exists; the wrapper returns 0. For a handle from the streaming loader, the C core's unload also stops and joins the prefetch thread and frees its shard buffers before freeing the handle (136.75).

#### infer.generate(h: modelhandle; prompt: str; maxtokens: i32): str!infererr

Autoregressive generation from `prompt`, at most `maxtokens` new tokens, returning only the continuation. `tk_infer_generate` in the core takes the limit and serialises generate and embed calls on one handle through a per-handle mutex. The wrapper takes all three arguments (137.11; it had no slot for the limit before) and always takes the `$err` arm.

#### infer.embed(h: modelhandle; text: str): @(f32)!infererr

Runs `text` through the embedding layer. `tk_infer_embed` exists; the wrapper always takes the `$err` arm. (Until 127.241 it returned an empty array through `$ok`, indistinguishable from a model whose embedding dimension is zero.)

#### infer.isloaded(h: modelhandle): bool

Whether `h` refers to a live model. Only `infer.load` can produce one: `infer.loadstreaming` never returns a handle (below), so a false answer here never means "a streaming load is still in progress".

#### infer.loadstreaming(modeldir: str; opts: streamopts): modelhandle!infererr

Streams layers from disk so only the active window is resident, trading latency for the ability to run a model larger than RAM.

**Incomplete: no build returns a handle (136.78).** Not implemented in the core either -- this one is unbuilt at both layers, not merely unwired. The core (`src/stdlib/infer_stream.c`) has the shard scanner, the RAM-bounded prefetch thread and the cleanup, but the layer loop that would load each shard into a llama.cpp model and context is missing. Until 136.78 a llama.cpp build (`TK_HAVE_LLAMACPP`) nevertheless returned **success** with a handle whose model and context were never set: `infer.isloaded` said false for it, and `generate` / `embed` on it could only fail, after a load that had reported success. The core now refuses instead, so there is no not-loaded handle to observe:

| Build | `modeldir` | Result |
|---|---|---|
| any | `""` | `$err`, `code` -1 |
| llama.cpp | a directory with no `layer_NNN.gguf` shard (or one that cannot be opened) | `$err`, `code` -1 |
| llama.cpp | a directory with shards | `$err`, `code` -2, `msg` "infer.loadstreaming: streaming load is not implemented (the layer loop is missing, story 136.78); use infer.load" |
| stub core (no llama.cpp) | non-empty | `$err`, `code` -2, `msg` names the missing llama.cpp |
| this repository's glue (every build today) | any | `$err`, `code` 0 (not linked, above) |

The prefetch machinery is kept in the file behind `TK_INFER_STREAM_LAYER_LOOP`, to be defined together with the loop. Real-model testing of the loop is behind the compute hold. Test: C2220.

### Notes on the intended design

- Models must be GGUF. The real implementation is conditional on `TK_HAVE_LLAMACPP`; without it the core itself returns `infererr{msg: "std.infer: llama.cpp not compiled in"; code: -1}`, which is a *different* failure from the glue stub and worth keeping distinguishable.
- `infer.embed` returns an array whose length is the model's embedding dimension. Check the error side before reading it.
- Concurrent `generate` and `embed` on the **same** handle are serialised internally; different handles proceed concurrently.

## Restoring it

A real `infer_glue.c`: six wrappers calling the four core functions (they already take the published arities, 135.31 / 137.11), plus constructor functions for the two options types. The type and field renames are already done (136.46). `loadstreaming` additionally needs a core. Nothing here is blocked on a language feature.

## See Also

- `std.mlx` -- the Apple Silicon sibling, in the same state and for the same reason.
- `std.llm` -- reaching a remote provider, and the working option today.
- `std.vecstore` -- where embeddings go once a module can produce them.
