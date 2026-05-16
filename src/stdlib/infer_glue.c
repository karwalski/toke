/*
 * infer_glue.c — i64-ABI wrappers for std.infer module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.infer.
 *
 * These functions require a local inference engine (llama.cpp / MLX).
 * On platforms without local inference support, they return error sentinels.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* infer.load(model_path) — load a local model, return handle or 0 */
int64_t tk_infer_load_w(int64_t model_path) {
    (void)model_path;
    /* Stub: local inference not linked */
    return 0;
}

/* infer.loadstreaming(model_path) — load model for streaming, return handle */
int64_t tk_infer_loadstreaming_w(int64_t model_path) {
    (void)model_path;
    return 0;
}

/* infer.unload(handle) — unload a loaded model */
int64_t tk_infer_unload_w(int64_t handle) {
    (void)handle;
    return 0;
}

/* infer.generate(handle, prompt) — generate text from prompt */
int64_t tk_infer_generate_w(int64_t handle, int64_t prompt) {
    (void)handle;
    (void)prompt;
    return (int64_t)(intptr_t)"[infer: not available]";
}

/* infer.embed(handle, text) — generate embedding vector, return as array */
int64_t tk_infer_embed_w(int64_t handle, int64_t text) {
    (void)handle;
    (void)text;
    /* Return empty array */
    int64_t *block = (int64_t *)malloc(sizeof(int64_t));
    if (!block) return 0;
    block[0] = 0;
    return (int64_t)(intptr_t)(block + 1);
}
