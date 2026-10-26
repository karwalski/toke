/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_INFER_HANDLE_H
#define TK_STDLIB_INFER_HANDLE_H

/*
 * infer_handle.h — PRIVATE layout of TkModelHandle, shared by infer.c and
 * infer_stream.c.  Not part of the std.infer C interface: callers see only
 * the opaque typedef in infer.h.
 *
 * Story 136.75.  Before this header, infer_stream.c carried its own copy of
 * the struct "which MUST match the one in infer.c exactly" -- and did not
 * (id[64] + a mutex named mu + a `loaded` flag + void* model/ctx, against
 * infer.c's id[128] + model + ctx + lock).  A handle made by
 * tk_infer_load_streaming and passed to tk_infer_unload was therefore read
 * through the wrong layout: unload freed the `loaded` flag as a llama_model
 * pointer and locked a mutex that lay past the end of the allocation.
 * One definition, two includers, no copy to drift.
 *
 * With TK_HAVE_LLAMACPP the struct names llama.cpp types only by pointer,
 * so the forward declarations below are enough; this header does not need
 * llama.h.
 */

#include "infer.h"

#ifdef TK_HAVE_LLAMACPP
#  include <pthread.h>
struct llama_model;
struct llama_context;
#endif

struct TkModelHandle {
    char id[128]; /* short human-readable label */
#ifdef TK_HAVE_LLAMACPP
    struct llama_model   *model; /* NULL for a streaming handle (72.7.3 TODO) */
    struct llama_context *ctx;   /* NULL for a streaming handle */
    pthread_mutex_t       lock;  /* serialises generate/embed on the handle */
#else
    int _placeholder; /* keep struct non-empty in stub builds */
#endif
};

#endif /* TK_STDLIB_INFER_HANDLE_H */
