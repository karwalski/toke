/*
 * infer_glue.c — i64-ABI wrappers for std.infer module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.infer.
 *
 * These functions require a local inference engine (llama.cpp / MLX).
 * On platforms without local inference support, they return error sentinels.
 */

#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "tk_runtime.h" /* 127.241: tk_current_error + tk_err_box (127.109) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 127.241: the error slot (runtime-abi.md §7.2).  Every `T!infererr` stub
 * below fails — no handle is ever produced — and until 127.241 they said so
 * with a bare 0 (so `e.msg` was RT005), or did not say so at all:
 * infer.generate returned the literal "[infer: not available]" and
 * infer.embed a zero-length array, both through the $ok arm, so a caller was
 * told the model had answered.  Each now stores an infererr record
 * {msg, code} (§7.3) and takes the $err arm.  code is 0 ("not applicable",
 * infer.h), which keeps the glue stub distinguishable from the core's own
 * "llama.cpp not compiled in" (-1) and "not implemented" (-2). */
static int64_t infer_unavailable(const char *msg) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)msg;
    box[1] = 0;                                 /* .code */
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* infer.load(model_path; opts) — load a local model, return handle.
 * 135.31 (1): takes the $inferopts record the .tki publishes; the wrapper had
 * no slot for it, so `infer.load(path;opts)` was E4026 and the options could
 * never be passed.  Still a stub: local inference is not linked. */
int64_t tk_infer_load_w(int64_t model_path, int64_t opts) {
    (void)model_path;
    (void)opts;
    return infer_unavailable("infer.load: local inference is not linked into this build (std.infer's glue is a stub)");
}

/* infer.loadstreaming(model_dir; opts) — load model for streaming, return
 * handle.  135.31 (1): takes the $streamopts record, as published. */
int64_t tk_infer_loadstreaming_w(int64_t model_path, int64_t opts) {
    (void)model_path;
    (void)opts;
    return infer_unavailable("infer.loadstreaming: local inference is not linked into this build (std.infer's glue is a stub)");
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
    return infer_unavailable("infer.generate: local inference is not linked into this build (std.infer's glue is a stub)");
}

/* infer.embed(handle, text) — generate embedding vector, return as array */
int64_t tk_infer_embed_w(int64_t handle, int64_t text) {
    (void)handle;
    (void)text;
    return infer_unavailable("infer.embed: local inference is not linked into this build (std.infer's glue is a stub)");
}

/* infer.isloaded(handle) — 131.38: declared in infer.tki but had no wrapper.
 * Consistent with the stub layer above: load never returns a handle, so
 * nothing is ever loaded. */
int64_t tk_infer_isloaded_w(int64_t handle) {
    (void)handle;
    return 0;
}
