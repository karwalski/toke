#ifndef TK_STDLIB_MLX_H
#define TK_STDLIB_MLX_H

/*
 * mlx.h — C interface for the std.mlx standard library module.
 *
 * Provides an optional MLX inference backend for Apple Silicon via a local
 * REST bridge process listening on localhost:MLX_BRIDGE_PORT (default 11438).
 *
 * Type mappings:
 *   str    = const char *  (null-terminated UTF-8)
 *   bool   = int           (0 = false, 1 = true)
 *   i32    = int32_t
 *   @(f32) = F32Array      (struct { float *data; uint64_t len; })
 *
 * On non-Apple platforms every function is a no-op stub; mlx_is_available()
 * always returns 0 and the load/generate/embed functions always return errors.
 *
 * malloc is permitted; callers own all returned heap pointers.
 * Call mlx_model_free() to release an MlxModel's heap strings.
 * Call mlx_result_free() / mlx_embed_result_free() as appropriate.
 *
 * No external dependencies beyond libc and POSIX sockets.
 *
 * Story: 72.6.3
 */

#include <stdint.h>

#ifndef MLX_BRIDGE_PORT
#define MLX_BRIDGE_PORT 11438
#endif

/* -------------------------------------------------------------------------
 * Array type for embedding vectors
 * ------------------------------------------------------------------------- */

/* F32Array — a heap-allocated array of single-precision floats.
 * The .data pointer is owned by the containing MlxEmbedResult. */
typedef struct {
    float   *data;
    uint64_t len;
} F32Array;

/* -------------------------------------------------------------------------
 * Model handle
 * ------------------------------------------------------------------------- */

/*
 * MlxModel — an opaque handle to a model loaded by the bridge.
 *   id   — unique string identifier assigned by the bridge
 *   path — file-system path from which the model was loaded
 * Both strings are heap-allocated; call mlx_model_free() when done.
 */
typedef struct {
    char *id;    /* heap-allocated */
    char *path;  /* heap-allocated */
} MlxModel;

/* mlx_model_free — release heap strings inside *m (does not free m itself). */
void mlx_model_free(MlxModel *m);

/* -------------------------------------------------------------------------
 * Result types
 * ------------------------------------------------------------------------- */

/*
 * MlxModelResult — result of mlx_load().
 * On success: is_err == 0, model is populated.
 * On failure: is_err != 0, err_msg is a heap-allocated error string.
 */
typedef struct {
    MlxModel    model;
    int         is_err;
    char       *err_msg; /* heap-allocated, NULL on success */
} MlxModelResult;

/*
 * MlxStrResult — result of mlx_generate().
 * On success: is_err == 0, text is a heap-allocated string.
 * On failure: is_err != 0, err_msg is a heap-allocated error string.
 */
typedef struct {
    char *text;    /* heap-allocated, NULL on error */
    int   is_err;
    char *err_msg; /* heap-allocated, NULL on success */
} MlxStrResult;

/*
 * MlxEmbedResult — result of mlx_embed().
 * On success: is_err == 0, embedding.data is a heap-allocated float array.
 * On failure: is_err != 0, err_msg is a heap-allocated error string.
 */
typedef struct {
    F32Array  embedding; /* .data is heap-allocated, NULL on error */
    int       is_err;
    char     *err_msg;   /* heap-allocated, NULL on success */
} MlxEmbedResult;

/* -------------------------------------------------------------------------
 * Memory management helpers
 * ------------------------------------------------------------------------- */

/* mlx_str_result_free — release heap strings inside *r. */
void mlx_str_result_free(MlxStrResult *r);

/* mlx_embed_result_free — release heap data inside *r. */
void mlx_embed_result_free(MlxEmbedResult *r);

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/*
 * mlx_is_available — return 1 if:
 *   (a) running on Apple Silicon (hw.optional.arm64 sysctl == 1), AND
 *   (b) the MLX bridge responds to GET http://localhost:MLX_BRIDGE_PORT/health
 *       with HTTP 200.
 * Returns 0 on all non-Apple platforms or when either condition fails.
 */
int mlx_is_available(void);

/*
 * mlx_load — ask the bridge to load the model at model_path.
 * Returns an MlxModelResult; caller must call mlx_model_free() on
 * result.model when done (even if result.is_err == 0).
 */
MlxModelResult mlx_load(const char *model_path);

/*
 * mlx_unload — ask the bridge to release the model identified by m.
 * Returns 1 on success, 0 on failure.
 */
int mlx_unload(const MlxModel *m);

/*
 * mlx_generate — generate up to max_tokens tokens from prompt using model m.
 * Returns an MlxStrResult; caller must call mlx_str_result_free() when done.
 */
MlxStrResult mlx_generate(const MlxModel *m, const char *prompt,
                           int32_t max_tokens);

/*
 * mlx_embed — compute a dense embedding for text using model m.
 * Returns an MlxEmbedResult; caller must call mlx_embed_result_free() when done.
 */
MlxEmbedResult mlx_embed(const MlxModel *m, const char *text);

#endif /* TK_STDLIB_MLX_H */
