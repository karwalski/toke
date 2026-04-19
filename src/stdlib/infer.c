/*
 * infer.c — Implementation of the std.infer standard library module.
 *
 * Provides in-process LLM inference by wrapping the llama.cpp C API.
 * All llama.cpp code is gated behind TK_HAVE_LLAMACPP.  When that macro is
 * not defined, every function returns a clear error so programs compiled
 * without llama.cpp still fail gracefully at runtime.
 *
 * When TK_HAVE_LLAMACPP IS defined, compile with llama.cpp headers on the
 * include path and link against -lllama.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own the returned TkModelHandle pointers and any
 * heap-allocated strings / float arrays documented in infer.h.
 *
 * Thread safety: a per-handle pthread_mutex_t serialises concurrent
 * generate/embed calls on the same handle.  Different handles may be used
 * concurrently without synchronisation.
 *
 * Story: 72.2.3
 */

#include "infer.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#ifdef TK_HAVE_LLAMACPP
#  include <pthread.h>
#  include "llama.h"   /* llama.cpp C API */
#endif

/* -------------------------------------------------------------------------
 * Internal handle definition (only meaningful with llama.cpp)
 * ------------------------------------------------------------------------- */

struct TkModelHandle {
    char id[128]; /* short human-readable label */
#ifdef TK_HAVE_LLAMACPP
    struct llama_model   *model;
    struct llama_context *ctx;
    pthread_mutex_t       lock;
#else
    int _placeholder; /* keep struct non-empty in stub builds */
#endif
};

/* -------------------------------------------------------------------------
 * Static error helpers
 * ------------------------------------------------------------------------- */

static TkInferErr make_err(const char *msg, int32_t code)
{
    TkInferErr e;
    e.msg  = msg;
    e.code = code;
    return e;
}

static TkModelHandleResult handle_err(const char *msg, int32_t code)
{
    TkModelHandleResult r;
    r.handle = NULL;
    r.is_err = 1;
    r.err    = make_err(msg, code);
    return r;
}

static TkGenerateResult gen_err(const char *msg, int32_t code)
{
    TkGenerateResult r;
    r.text   = NULL;
    r.is_err = 1;
    r.err    = make_err(msg, code);
    return r;
}

static TkEmbedResult embed_err(const char *msg, int32_t code)
{
    TkEmbedResult r;
    r.data   = NULL;
    r.len    = 0;
    r.is_err = 1;
    r.err    = make_err(msg, code);
    return r;
}

/* -------------------------------------------------------------------------
 * tk_infer_load
 * ------------------------------------------------------------------------- */

TkModelHandleResult tk_infer_load(const char *model_path, TkInferOpts opts)
{
    if (!model_path || model_path[0] == '\0') {
        return handle_err("infer.load: model_path must not be empty", -3);
    }

#ifndef TK_HAVE_LLAMACPP
    (void)opts;
    return handle_err("std.infer: llama.cpp not compiled in", -1);
#else
    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = opts.n_gpu_layers;

    struct llama_model *model = llama_load_model_from_file(model_path, mparams);
    if (!model) {
        return handle_err("infer.load: llama_load_model_from_file failed", -4);
    }

    struct llama_context_params cparams = llama_context_default_params();
    if (opts.n_threads > 0) {
        cparams.n_threads      = (uint32_t)opts.n_threads;
        cparams.n_threads_batch = (uint32_t)opts.n_threads;
    }
    if (opts.seed != -1) {
        cparams.seed = (uint32_t)opts.seed;
    }

    struct llama_context *ctx = llama_new_context_with_model(model, cparams);
    if (!ctx) {
        llama_free_model(model);
        return handle_err("infer.load: llama_new_context_with_model failed", -5);
    }

    TkModelHandle *h = (TkModelHandle *)malloc(sizeof(TkModelHandle));
    if (!h) {
        llama_free(ctx);
        llama_free_model(model);
        return handle_err("infer.load: out of memory", -6);
    }

    /* Build a short id from the final path component */
    const char *base = strrchr(model_path, '/');
    base = base ? base + 1 : model_path;
    snprintf(h->id, sizeof(h->id), "%s", base);

    h->model = model;
    h->ctx   = ctx;
    pthread_mutex_init(&h->lock, NULL);

    TkModelHandleResult r;
    r.handle = h;
    r.is_err = 0;
    r.err    = make_err(NULL, 0);
    return r;
#endif /* TK_HAVE_LLAMACPP */
}

/* -------------------------------------------------------------------------
 * tk_infer_unload
 * ------------------------------------------------------------------------- */

int tk_infer_unload(TkModelHandle *h)
{
    if (!h) return 0;

#ifdef TK_HAVE_LLAMACPP
    pthread_mutex_lock(&h->lock);
    if (h->ctx) {
        llama_free(h->ctx);
        h->ctx = NULL;
    }
    if (h->model) {
        llama_free_model(h->model);
        h->model = NULL;
    }
    pthread_mutex_unlock(&h->lock);
    pthread_mutex_destroy(&h->lock);
#endif

    free(h);
    return 1;
}

/* -------------------------------------------------------------------------
 * tk_infer_generate
 * ------------------------------------------------------------------------- */

TkGenerateResult tk_infer_generate(TkModelHandle *h,
                                    const char    *prompt,
                                    int32_t        max_tokens)
{
    if (!h) {
        return gen_err("infer.generate: null model handle", -3);
    }
    if (!prompt) {
        return gen_err("infer.generate: null prompt", -3);
    }
    if (max_tokens <= 0) {
        return gen_err("infer.generate: max_tokens must be positive", -3);
    }

#ifndef TK_HAVE_LLAMACPP
    return gen_err("std.infer: llama.cpp not compiled in", -1);
#else
    pthread_mutex_lock(&h->lock);

    if (!h->model || !h->ctx) {
        pthread_mutex_unlock(&h->lock);
        return gen_err("infer.generate: model is not loaded", -7);
    }

    /* Tokenise the prompt */
    int n_prompt_max = (int)strlen(prompt) + 16;
    llama_token *tokens = (llama_token *)malloc(
        (size_t)n_prompt_max * sizeof(llama_token));
    if (!tokens) {
        pthread_mutex_unlock(&h->lock);
        return gen_err("infer.generate: out of memory (tokenise)", -6);
    }

    int n_tokens = llama_tokenize(h->model, prompt, (int)strlen(prompt),
                                  tokens, n_prompt_max,
                                  /*add_bos=*/1, /*special=*/0);
    if (n_tokens < 0) {
        free(tokens);
        pthread_mutex_unlock(&h->lock);
        return gen_err("infer.generate: llama_tokenize failed", -8);
    }

    /* Build batch and decode prompt */
    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens, 0, 0);
    if (llama_decode(h->ctx, batch) != 0) {
        free(tokens);
        pthread_mutex_unlock(&h->lock);
        return gen_err("infer.generate: llama_decode (prompt) failed", -9);
    }
    free(tokens);

    /* Greedy sampling loop */
    /* Output buffer: each token may produce up to 8 bytes of UTF-8; allocate
     * generously and realloc if needed. */
    size_t buf_cap = (size_t)(max_tokens * 8 + 1);
    char  *buf     = (char *)malloc(buf_cap);
    if (!buf) {
        pthread_mutex_unlock(&h->lock);
        return gen_err("infer.generate: out of memory (output buffer)", -6);
    }
    size_t buf_len = 0;
    buf[0] = '\0';

    int n_cur = n_tokens;
    for (int i = 0; i < max_tokens; i++) {
        llama_token new_tok = llama_sample_token_greedy(h->ctx, NULL);

        if (new_tok == llama_token_eos(h->model)) {
            break;
        }

        /* Decode token to text */
        char piece[32];
        int piece_len = llama_token_to_piece(h->model, new_tok,
                                              piece, (int)sizeof(piece), 0);
        if (piece_len < 0) piece_len = 0;

        /* Grow buffer if needed */
        if (buf_len + (size_t)piece_len + 1 > buf_cap) {
            buf_cap  = buf_cap * 2 + (size_t)piece_len + 1;
            char *nb = (char *)realloc(buf, buf_cap);
            if (!nb) {
                free(buf);
                pthread_mutex_unlock(&h->lock);
                return gen_err("infer.generate: out of memory (realloc)", -6);
            }
            buf = nb;
        }

        memcpy(buf + buf_len, piece, (size_t)piece_len);
        buf_len += (size_t)piece_len;
        buf[buf_len] = '\0';

        /* Feed the new token back */
        struct llama_batch next_batch = llama_batch_get_one(&new_tok, 1,
                                                             n_cur, 0);
        n_cur++;
        if (llama_decode(h->ctx, next_batch) != 0) {
            break;
        }
    }

    pthread_mutex_unlock(&h->lock);

    TkGenerateResult r;
    r.text   = buf;
    r.is_err = 0;
    r.err    = make_err(NULL, 0);
    return r;
#endif /* TK_HAVE_LLAMACPP */
}

/* -------------------------------------------------------------------------
 * tk_infer_embed
 * ------------------------------------------------------------------------- */

TkEmbedResult tk_infer_embed(TkModelHandle *h, const char *text)
{
    if (!h) {
        return embed_err("infer.embed: null model handle", -3);
    }
    if (!text) {
        return embed_err("infer.embed: null text", -3);
    }

#ifndef TK_HAVE_LLAMACPP
    return embed_err("std.infer: llama.cpp not compiled in", -1);
#else
    pthread_mutex_lock(&h->lock);

    if (!h->model || !h->ctx) {
        pthread_mutex_unlock(&h->lock);
        return embed_err("infer.embed: model is not loaded", -7);
    }

    /* Tokenise */
    int n_max = (int)strlen(text) + 16;
    llama_token *tokens = (llama_token *)malloc(
        (size_t)n_max * sizeof(llama_token));
    if (!tokens) {
        pthread_mutex_unlock(&h->lock);
        return embed_err("infer.embed: out of memory (tokenise)", -6);
    }

    int n_tokens = llama_tokenize(h->model, text, (int)strlen(text),
                                  tokens, n_max,
                                  /*add_bos=*/1, /*special=*/0);
    if (n_tokens < 0) {
        free(tokens);
        pthread_mutex_unlock(&h->lock);
        return embed_err("infer.embed: llama_tokenize failed", -8);
    }

    struct llama_batch batch = llama_batch_get_one(tokens, n_tokens, 0, 0);
    if (llama_decode(h->ctx, batch) != 0) {
        free(tokens);
        pthread_mutex_unlock(&h->lock);
        return embed_err("infer.embed: llama_decode failed", -9);
    }
    free(tokens);

    /* Extract embeddings from the last token position */
    int n_embd = llama_n_embd(h->model);
    if (n_embd <= 0) {
        pthread_mutex_unlock(&h->lock);
        return embed_err("infer.embed: model reports zero embedding dimension", -10);
    }

    const float *src = llama_get_embeddings(h->ctx);
    if (!src) {
        pthread_mutex_unlock(&h->lock);
        return embed_err("infer.embed: llama_get_embeddings returned NULL", -11);
    }

    float *out = (float *)malloc((size_t)n_embd * sizeof(float));
    if (!out) {
        pthread_mutex_unlock(&h->lock);
        return embed_err("infer.embed: out of memory (embedding copy)", -6);
    }
    memcpy(out, src, (size_t)n_embd * sizeof(float));

    pthread_mutex_unlock(&h->lock);

    TkEmbedResult r;
    r.data   = out;
    r.len    = (uint32_t)n_embd;
    r.is_err = 0;
    r.err    = make_err(NULL, 0);
    return r;
#endif /* TK_HAVE_LLAMACPP */
}

/* -------------------------------------------------------------------------
 * tk_infer_is_loaded
 * ------------------------------------------------------------------------- */

int tk_infer_is_loaded(const TkModelHandle *h)
{
    if (!h) return 0;
#ifdef TK_HAVE_LLAMACPP
    return (h->model != NULL && h->ctx != NULL) ? 1 : 0;
#else
    /* In stub builds a handle is never validly constructed, so always 0. */
    return 0;
#endif
}

/* -------------------------------------------------------------------------
 * tk_infer_load_streaming  (Epic 72.7 — not yet implemented)
 * ------------------------------------------------------------------------- */

TkModelHandleResult tk_infer_load_streaming(const char  *model_dir,
                                              TkStreamOpts opts)
{
    (void)model_dir;
    (void)opts;
    return handle_err(
        "infer.load_streaming: not yet implemented (Epic 72.7)", -2);
}
