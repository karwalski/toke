#ifndef TK_STDLIB_INFER_H
#define TK_STDLIB_INFER_H

/*
 * infer.h — C interface for the std.infer standard library module.
 *
 * Provides in-process LLM inference via llama.cpp.  All llama.cpp code is
 * gated behind TK_HAVE_LLAMACPP.  When that macro is not defined, every
 * function returns an error result so callers fail gracefully at runtime.
 *
 * Type mappings:
 *   Str   = const char *  (null-terminated UTF-8)
 *   i32   = int32_t
 *   f32   = float
 *   bool  = int  (0 = false, 1 = true)
 *
 * Ownership:
 *   - tk_infer_load / tk_infer_load_streaming return a heap-allocated
 *     TkModelHandle on success; caller must pass it to tk_infer_unload.
 *   - tk_infer_embed returns a heap-allocated float array in result.data;
 *     caller must free it when done.
 *   - All const char * fields inside result structs point to static strings
 *     or strings owned by the handle; do not free them independently.
 *
 * Thread safety:
 *   Concurrent generate/embed calls on the same handle are serialised via
 *   a per-handle pthread_mutex_t.  Different handles may be used concurrently.
 *
 * Story: 72.2.1–72.2.7
 */

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------- */

/* TkModelHandle — opaque handle to a loaded model.
 * The id field is a short human-readable label assigned at load time. */
typedef struct TkModelHandle TkModelHandle;

/* TkInferOpts — load-time tuning parameters. */
typedef struct {
    int32_t n_gpu_layers; /* layers to offload to GPU (0 = CPU only) */
    int32_t n_threads;    /* CPU threads (0 = llama.cpp default) */
    int32_t seed;         /* RNG seed (-1 = random) */
} TkInferOpts;

/* TkStreamOpts — options for disk-streaming model load (Epic 72.7). */
typedef struct {
    float   ram_ceiling_gb;  /* maximum RAM the streaming loader may use */
    int32_t prefetch_layers; /* layers to prefetch ahead of decode window */
    int     requires_nvme;   /* 1 = refuse to stream from spinning disk */
} TkStreamOpts;

/* TkInferErr — error information returned by fallible operations. */
typedef struct {
    const char *msg;  /* human-readable description; static lifetime */
    int32_t     code; /* internal error code (0 if not applicable) */
} TkInferErr;

/* TkModelHandleResult — result of a load operation. */
typedef struct {
    TkModelHandle *handle; /* non-NULL on success */
    int            is_err;
    TkInferErr     err;
} TkModelHandleResult;

/* TkGenerateResult — result of a generate operation. */
typedef struct {
    const char *text;  /* heap-allocated generated text; caller must free */
    int         is_err;
    TkInferErr  err;
} TkGenerateResult;

/* TkEmbedResult — result of an embed operation. */
typedef struct {
    float      *data;   /* heap-allocated float array; caller must free */
    uint32_t    len;    /* number of floats in data */
    int         is_err;
    TkInferErr  err;
} TkEmbedResult;

/* -------------------------------------------------------------------------
 * Functions
 * ------------------------------------------------------------------------- */

/* infer.load(model_path:Str; opts:infer_opts) -> model_handle!infererr
 *
 * Loads a GGUF model from model_path.  By convention models live under
 * ~/.loke/models/.  Returns a heap-allocated TkModelHandle on success.
 * The caller must eventually call tk_infer_unload to free resources. */
TkModelHandleResult tk_infer_load(const char *model_path,
                                   TkInferOpts opts);

/* infer.unload(h:model_handle) -> bool
 *
 * Frees all resources owned by h, including the handle itself.  Returns 1
 * on success, 0 if h is NULL or already unloaded.  Do not use h after this
 * call. */
int tk_infer_unload(TkModelHandle *h);

/* infer.generate(h:model_handle; prompt:Str; max_tokens:i32) -> Str!infererr
 *
 * Generates up to max_tokens new tokens from prompt.  Returns the generated
 * text only (prompt not included).  result.text is heap-allocated; caller
 * must free it.  Calls on the same handle are serialised internally. */
TkGenerateResult tk_infer_generate(TkModelHandle *h,
                                    const char    *prompt,
                                    int32_t        max_tokens);

/* infer.embed(h:model_handle; text:Str) -> @(f32)!infererr
 *
 * Encodes text through the model's embedding layer and returns a
 * heap-allocated float array.  result.len is the embedding dimension.
 * The caller must free result.data when done. */
TkEmbedResult tk_infer_embed(TkModelHandle *h, const char *text);

/* infer.is_loaded(h:model_handle) -> bool
 *
 * Returns 1 if h refers to an active, loaded model; 0 otherwise. */
int tk_infer_is_loaded(const TkModelHandle *h);

/* infer.load_streaming(model_dir:Str; opts:stream_opts) -> model_handle!infererr
 *
 * Epic 72.7 extension — disk-streaming load so only the active layer window
 * needs to reside in RAM.  NOT YET IMPLEMENTED: always returns is_err=1 with
 * code=-2. */
TkModelHandleResult tk_infer_load_streaming(const char  *model_dir,
                                              TkStreamOpts opts);

#endif /* TK_STDLIB_INFER_H */
