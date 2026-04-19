/*
 * infer_stream.h — disk-streaming extension for the std.infer module.
 *
 * This header declares the streaming-specific implementation functions that
 * back the infer.load_streaming and infer.detect_storage_type toke API.
 *
 * These declarations are intentionally kept separate from infer.h so that
 * infer_stream.c can be compiled and linked independently alongside infer.c.
 *
 * All public symbols are gated behind TK_HAVE_LLAMACPP.  Without that macro,
 * every function returns an error result so callers fail gracefully.
 *
 * Type mappings:
 *   Str   = const char *  (null-terminated UTF-8)
 *   i32   = int32_t
 *   f32   = float
 *   bool  = int  (0 = false, 1 = true)
 *
 * Thread safety:
 *   tk_infer_load_streaming itself is thread-safe: each call creates an
 *   independent TkModelHandle with its own mutex and prefetch state.
 *   The prefetch thread created per load is joined on tk_infer_unload.
 *
 * Story: 72.7.1–72.7.5
 */

#ifndef TK_STDLIB_INFER_STREAM_H
#define TK_STDLIB_INFER_STREAM_H

#include "infer.h"

/* -------------------------------------------------------------------------
 * Storage-type detection (Story 72.7.2)
 * -------------------------------------------------------------------------
 *
 * infer.detect_storage_type(path:Str) -> Str
 *
 * Returns one of: "nvme", "ssd", "hdd", "unknown".
 *
 * On macOS: uses IOKit / diskutil info to identify the underlying transport.
 *   NVMe devices report "NVMe" in the protocol field; SATA SSDs report no
 *   rotational media.  Spinning disks are classified "hdd".
 *
 * On Linux: reads /sys/block/<dev>/queue/rotational (0 = solid-state) and
 *   /sys/block/<dev>/device/transport ("nvme" string present = nvme).
 *
 * The returned string has static storage duration; do not free it.
 */
const char *tk_infer_detect_storage_type(const char *path);

/* -------------------------------------------------------------------------
 * Disk-streaming load (Story 72.7.3)
 * -------------------------------------------------------------------------
 *
 * infer.load_streaming(model_dir:Str, opts:$stream_opts) -> model_handle!
 *
 * Loads a sharded model from model_dir in a memory-efficient streaming
 * fashion.  The directory must contain per-layer GGUF shards named:
 *   layer_000.gguf, layer_001.gguf, …
 *
 * Behaviour:
 *   - Detects storage type; warns to stderr if requires_nvme is set and the
 *     storage type is not "nvme".
 *   - Loads one shard at a time: load -> process -> unload.
 *   - A background pthread prefetches the next prefetch_layers shards into
 *     anonymous memory-mapped buffers to overlap I/O with compute.
 *   - RAM usage is tracked; the loader refuses to prefetch a shard that would
 *     push usage above ram_ceiling_gb.
 *   - Throughput is sampled in tokens/second; if it falls below 0.1 tok/s,
 *     a warning is emitted to stderr.
 *
 * Returns a heap-allocated TkModelHandle on success.  The caller must
 * eventually pass the handle to tk_infer_unload, which joins the prefetch
 * thread and releases all resources.
 *
 * Without TK_HAVE_LLAMACPP: always returns is_err=1, code=-2.
 */
TkModelHandleResult tk_infer_load_streaming(const char  *model_dir,
                                              TkStreamOpts opts);

/* -------------------------------------------------------------------------
 * Throughput monitor (Story 72.7.4)
 * -------------------------------------------------------------------------
 *
 * TkStreamThroughput — snapshot of current streaming throughput.
 *
 * tokens_per_sec  — tokens processed per second (averaged over last window).
 * bytes_loaded    — total bytes read from disk so far.
 * ram_used_bytes  — current RAM consumed by live shard buffers.
 * warn_slow       — 1 if tokens_per_sec < TK_STREAM_SLOW_THRESHOLD_TOK_S.
 */
typedef struct {
    double   tokens_per_sec;
    uint64_t bytes_loaded;
    uint64_t ram_used_bytes;
    int      warn_slow;
} TkStreamThroughput;

/* Threshold below which a slow-stream warning is emitted. */
#define TK_STREAM_SLOW_THRESHOLD_TOK_S 0.1

/*
 * tk_infer_stream_throughput — query the current throughput metrics for an
 * active streaming handle.
 *
 * Returns a zeroed TkStreamThroughput if h is NULL or the handle was not
 * created by tk_infer_load_streaming.
 */
TkStreamThroughput tk_infer_stream_throughput(const TkModelHandle *h);

#endif /* TK_STDLIB_INFER_STREAM_H */
