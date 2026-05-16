/*
 * mlx_glue.c — i64-ABI wrappers for std.mlx module.
 *
 * MLX (Apple's ML framework) bindings for local model inference on Apple
 * Silicon. On non-Apple platforms or when MLX is not available, these
 * return error sentinels.
 */

#include <stdint.h>

/* mlx.isavailable() — return 1 if MLX runtime is available */
int64_t tk_mlx_isavailable_w(int64_t dummy) {
    (void)dummy;
#if defined(__APPLE__) && defined(__arm64__)
    /* MLX is theoretically available on Apple Silicon,
     * but actual availability depends on linked framework.
     * Return 0 until MLX is properly integrated. */
    return 0;
#else
    return 0;
#endif
}

/* mlx.load(model_path) — load an MLX model, return handle or 0 */
int64_t tk_mlx_load_w(int64_t model_path) {
    (void)model_path;
    return 0;
}

/* mlx.generate(handle, prompt) — generate text */
int64_t tk_mlx_generate_w(int64_t handle, int64_t prompt) {
    (void)handle;
    (void)prompt;
    return (int64_t)(intptr_t)"[mlx: not available]";
}
