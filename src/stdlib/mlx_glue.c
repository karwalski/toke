/* SPDX-License-Identifier: MIT */
/*
 * mlx_glue.c — i64-ABI wrappers for std.mlx module.
 *
 * MLX (Apple's ML framework) bindings for local model inference on Apple
 * Silicon. On non-Apple platforms or when MLX is not available, these
 * return error sentinels.
 */

#include "tk_runtime.h"   /* 127.241: tk_current_error + tk_err_box (127.109) */
#include <stdint.h>

/* 127.241: the error slot (runtime-abi.md §7.2).  The `T!mlxerr` stubs below
 * always fail, and until 127.241 they said so with a bare 0 (`e.msg` was
 * RT005) — or not at all: mlx.generate returned the literal
 * "[mlx: not available]" through the $ok arm.  Each now stores an mlxerr
 * record {msg} (§7.3) and takes the $err arm. */
static int64_t mlx_unavailable(const char *msg) {
    int64_t *box = (int64_t *)tk_err_box((int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)msg;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* mlx.isavailable() — 137.11: false, on every platform, because nothing in
 * this file reaches the core: load/generate/embed below are stubs that always
 * fail, so reporting the bridge as available (mlx_is_available() in mlx.c)
 * would promise a capability no call can deliver.  The two identical #if arms
 * that said the same thing are gone; wire the core and this together. */
int64_t tk_mlx_isavailable_w(void) {
    return 0;
}

/* mlx.unload(m) -> bool — 137.11: declared in stdlib/mlx.tki with no wrapper,
 * so a call failed at link (E9003).  load never hands out a model, so there is
 * never anything to unload: false, not a claimed success. */
int64_t tk_mlx_unload_w(int64_t handle) {
    (void)handle;
    return 0;
}

/* mlx.load(model_path) — load an MLX model, return handle or 0 */
int64_t tk_mlx_load_w(int64_t model_path) {
    (void)model_path;
    return mlx_unavailable("mlx.load: MLX is not available (std.mlx's glue is a stub)");
}

/* mlx.generate(handle; prompt; maxtokens) — generate text.
 * 137.11 / 121.53 (5): takes the i32 token limit the .tki publishes (the core
 * mlx_generate takes max_tokens and sends it to the bridge, so the interface
 * was right and the wrapper had no slot).  Still a stub: always $err. */
int64_t tk_mlx_generate_w(int64_t handle, int64_t prompt, int64_t maxtokens) {
    (void)handle;
    (void)prompt;
    (void)maxtokens;
    return mlx_unavailable("mlx.generate: MLX is not available (std.mlx's glue is a stub)");
}

/* mlx.embed(m; text) -> @(f32)!mlxerr — 127.241.  Declared in
 * stdlib/mlx.tki with no wrapper at all, so the export was quarantined and a
 * call failed to link.  Consistent with the stub layer above: load never
 * hands out a model, so there is nothing to embed with. */
int64_t tk_mlx_embed_w(int64_t handle, int64_t text) {
    (void)handle;
    (void)text;
    return mlx_unavailable("mlx.embed: MLX is not available (std.mlx's glue is a stub)");
}
