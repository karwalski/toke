---
title: std.infer
slug: infer
section: reference/stdlib
order: 45
---

> **Status: FAÇADE -- do not write against this page's API.** All six published functions have a `_w` wrapper, so a call links; every one of those wrappers is a stub that ignores its arguments. `infer.load` returns 0 unconditionally, so no handle is ever produced, so nothing else can do anything useful. `src/stdlib/infer.c` is 397 lines implementing `tk_infer_load`, `tk_infer_unload`, `tk_infer_generate` and `tk_infer_embed`; `src/stdlib/infer_glue.c` calls none of them. Same class as story 136.44 (`std.tls`) and 136.16 (`std.toon`): a finished core behind glue that returns 0.

`std.infer` is intended to provide in-process LLM inference via llama.cpp. Models are GGUF files (by convention under `~/.loke/models/`) reached through an opaque handle, with text generation, embedding extraction, and a disk-streaming loader for machines that cannot hold the whole model in RAM.

## What is actually reachable today

Measured against the built compiler, not read off the interface. Every row links -- this module's failure mode is quieter than `std.mlx`'s, because nothing errors, it simply does nothing.

| Published call | `.tki` arity | Glue arity | Behaviour today |
|---|---|---|---|
| `infer.load` | 2 | **1** | drops `infer_opts`; returns 0, never a handle |
| `infer.loadstreaming` | 2 | **1** | drops `stream_opts`; returns 0 |
| `infer.unload` | 1 | 1 | returns 0 |
| `infer.generate` | 3 | **2** | drops the token limit; returns the literal `"[infer: not available]"` |
| `infer.embed` | 2 | 2 | returns a zero-length array |
| `infer.isloaded` | 1 | 1 | returns 0. Added by 131.38, which found it declared with no wrapper at all -- and made it a stub consistent with the rest rather than a working function, because `load` never hands out a handle for it to check |

A call written the way the interface publishes it is rejected before the linker:

```
E4026 wrong number of arguments for 'std.infer.generate':
      the implementation takes 2 arguments, the call passes 3
```

The three arity drops are not cosmetic. `infer_opts` carries the GPU-layer count, the thread count and the RNG seed; `stream_opts` carries the RAM ceiling and the prefetch depth; `generate`'s third argument is the token limit. A restored module that kept the current wrapper signatures would compile every caller and honour none of their configuration -- which is exactly the defect class Epic 136 was opened to close.

Because `isloaded` is the only call whose stub answer is *honest* (nothing is loaded, and that is true), the single compiling example below is a guard, and that is all it can be.

```toke
m=inferprobe;
i=infer:std.infer;
i=io:std.io;

f=main():i64{
  (* load is a stub that returns 0, so this handle is never valid and
     isloaded is correspondingly false. The else branch is what runs. *)
  let h=infer.load("/models/mistral-7b.Q4KM.gguf");
  if(infer.isloaded(h)){
    io.println("model loaded")
  }el{
    io.println("no local inference: std.infer glue is a stub")
  };
  <0
};
```

## The underscore problem, on top of the glue problem

Several published names cannot be written in a toke program at all, because toke's default 59-character profile excludes `_`:

| Name | Kind | Why it cannot be written |
|---|---|---|
| `model_handle` | type | `$model_handle` will not lex |
| `infer_opts` | type | `$infer_opts{...}` will not lex |
| `stream_opts` | type | `$stream_opts{...}` will not lex |
| `n_gpu_layers`, `n_threads` | field | cannot be named in a struct literal or a field read |
| `ram_ceiling_gb`, `prefetch_layers`, `requires_nvme` | field | as above |
| `infer.load_streaming`, `infer.is_loaded` | call-name | as this page previously documented them. The interface already spells them `loadstreaming` and `isloaded`; the prose had not caught up |

So the options types are doubly unreachable: no wrapper takes them, *and* no toke program could construct one if a wrapper did. Restoring the module needs the `std.tls` treatment from 136.44 -- constructor functions (`infer.opts(gpulayers; threads; seed)`) instead of struct literals, and handles kept opaque as `i64`.

## The design, kept as the record

Everything below describes what `src/stdlib/infer.c` already implements and what a restored module should expose. **It is not callable as written.**

### Types

#### modelhandle

An opaque handle to a loaded model, carrying the model instance id. Published today as `model_handle`, which is unspellable.

#### inferopts

How to load: GPU layers to offload (0 = CPU only), CPU threads (0 = the llama.cpp heuristic), RNG seed (-1 = random). Published today as `infer_opts` with underscore fields.

#### streamopts

Disk-streaming load (Epic 72.7): RAM ceiling in gigabytes, layers to prefetch ahead of the decode window, and whether to refuse a spinning disk. Published today as `stream_opts` with underscore fields.

#### infererr

`msg: str` and `code: i32`.

### Functions

#### infer.load(modelpath: str; opts: inferopts): modelhandle!infererr

Loads a GGUF file into memory. `tk_infer_load` in the core does this and takes the options; the wrapper takes the path alone and returns 0.

#### infer.unload(h: modelhandle): bool

Frees the model. `tk_infer_unload` exists; the wrapper returns 0.

#### infer.generate(h: modelhandle; prompt: str; maxtokens: i32): str!infererr

Autoregressive generation from `prompt`, at most `maxtokens` new tokens, returning only the continuation. `tk_infer_generate` in the core takes the limit and serialises generate and embed calls on one handle through a per-handle mutex. The wrapper takes two arguments and returns a literal.

#### infer.embed(h: modelhandle; text: str): @(f32)!infererr

Runs `text` through the embedding layer. `tk_infer_embed` exists; the wrapper returns an empty array, which is indistinguishable from a model whose embedding dimension is zero.

#### infer.isloaded(h: modelhandle): bool

Whether `h` refers to a live model.

#### infer.loadstreaming(modeldir: str; opts: streamopts): modelhandle!infererr

Streams layers from disk so only the active window is resident, trading latency for the ability to run a model larger than RAM. Not implemented in the core either -- this one is unbuilt at both layers, not merely unwired.

### Notes on the intended design

- Models must be GGUF. The real implementation is conditional on `TK_HAVE_LLAMACPP`; without it the core itself returns `infererr{msg: "std.infer: llama.cpp not compiled in"; code: -1}`, which is a *different* failure from the glue stub and worth keeping distinguishable.
- `infer.embed` returns an array whose length is the model's embedding dimension. Check the error side before reading it.
- Concurrent `generate` and `embed` on the **same** handle are serialised internally; different handles proceed concurrently.

## Restoring it

A real `infer_glue.c`: six wrappers calling the four core functions, widened to the published arities, plus constructor functions for the two options types and a rename of the three underscore type names. `loadstreaming` additionally needs a core. Nothing here is blocked on a language feature.

## See Also

- `std.mlx` -- the Apple Silicon sibling, in the same state and for the same reason.
- `std.llm` -- reaching a remote provider, and the working option today.
- `std.vecstore` -- where embeddings go once a module can produce them.
