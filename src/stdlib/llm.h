#ifndef TK_STDLIB_LLM_H
#define TK_STDLIB_LLM_H

/*
 * llm.h — C interface for the std.llm standard library module.
 *
 * Type mappings:
 *   Str  = const char *  (null-terminated UTF-8)
 *   bool = int  (0 = false, 1 = true)
 *   u64  = uint64_t
 *
 * Provides an OpenAI-compatible chat completion client over plain HTTP/1.1
 * using POSIX sockets.  HTTPS requires TLS support compiled in (see llm.c).
 *
 * malloc is permitted; callers own returned heap pointers unless noted.
 *
 * No external dependencies beyond libc and POSIX sockets.
 *
 * Story: 17.1.1
 */

#include <stdint.h>

/* TkLlmClient — holds connection parameters for one LLM provider endpoint. */
typedef struct {
    const char *base_url;    /* e.g. "https://api.openai.com/v1" or "http://localhost:11434/v1" */
    const char *api_key;     /* Bearer token, may be NULL for local */
    const char *model;       /* e.g. "gpt-4o", "claude-3-5-sonnet-20241022" */
    uint32_t    timeout_ms;  /* 0 = 30000 */
    uint32_t    max_retries; /* 0 = 3 */
    /* Story 32.1.1 — retry backoff */
    uint64_t    retry_base_delay_ms;   /* base delay for exponential backoff; 0 = disabled */
    uint64_t    retry_max_retries;     /* max retries for backoff; 0 = use max_retries */
    /* Story 32.1.2 — cumulative usage tracking */
    uint64_t    total_input_tokens;
    uint64_t    total_output_tokens;
} TkLlmClient;

/* TkLlmMsg — a single message in a conversation turn. */
typedef struct {
    const char *role;     /* "system", "user", "assistant" */
    const char *content;
} TkLlmMsg;

/* TkLlmResp — result of a non-streaming chat completion. */
typedef struct {
    const char *content;      /* full response text; heap-allocated */
    uint64_t    input_tokens;
    uint64_t    output_tokens;
    int         is_err;
    const char *err_msg;      /* heap-allocated on error, NULL otherwise */
} TkLlmResp;

/* TkLlmStream — collected delta chunks from a streaming completion. */
typedef struct {
    const char **chunks;   /* NULL-terminated array of delta strings; heap-allocated */
    uint64_t     nchunks;
    int          is_err;
    const char  *err_msg;  /* heap-allocated on error, NULL otherwise */
} TkLlmStream;

/* llm_client — allocate a new TkLlmClient.  Caller must call llm_client_free. */
TkLlmClient *llm_client(const char *base_url, const char *api_key, const char *model);

/* llm_client_free — release all resources owned by c. */
void         llm_client_free(TkLlmClient *c);

/* llm_chat — send msgs to the model and return a single completion.
 * temperature 0.0–2.0; typical default 0.7. */
TkLlmResp    llm_chat(TkLlmClient *c, TkLlmMsg *msgs, uint64_t nmsgs, double temperature);

/* llm_chatstream — send msgs with stream:true, collect all SSE delta chunks. */
TkLlmStream  llm_chatstream(TkLlmClient *c, TkLlmMsg *msgs, uint64_t nmsgs, double temperature);

/* llm_streamnext — return s->chunks[*idx] and advance *idx.
 * Returns NULL when all chunks have been consumed. */
const char  *llm_streamnext(TkLlmStream *s, uint64_t *idx);

/* llm_complete — convenience wrapper: sends prompt as a single user message. */
TkLlmResp    llm_complete(TkLlmClient *c, const char *prompt, double temperature);

/* llm_countokens — approximate token count: strlen(text) / 4. */
uint64_t     llm_countokens(TkLlmClient *c, const char *text);

/* llm_build_request — build an OpenAI-format JSON request body.
 * Returns a heap-allocated string the caller must free, or NULL on error. */
const char  *llm_build_request(TkLlmMsg *msgs, uint64_t nmsgs, const char *model,
                                double temperature, int stream);

/* llm_parse_sse_chunk — extract choices[0].delta.content from a raw SSE data JSON string.
 * Pass the raw JSON after "data: ".
 * Returns a heap-allocated string (possibly empty "") for a normal delta.
 * Returns NULL for "[DONE]" or on parse error. */
const char  *llm_parse_sse_chunk(const char *sse_data);

/* -----------------------------------------------------------------------
 * Story 32.1.1 — Embeddings and retry backoff
 * ----------------------------------------------------------------------- */

/* F64Array — a read-only view of a heap-allocated array of doubles.
 * The .data pointer is owned by the containing result struct. */
typedef struct { const double *data; uint64_t len; } F64Array;

/* F64ArrayLlmResult — result of a single embedding request. */
typedef struct {
    F64Array    ok;
    int         is_err;
    const char *err_msg;  /* heap-allocated on error, NULL otherwise */
} F64ArrayLlmResult;

/* F64ArraysLlmResult — result of a batch embedding request. */
typedef struct {
    F64Array   *data;     /* heap-allocated array of F64Array; each .data is heap-allocated */
    uint64_t    len;
    int         is_err;
    const char *err_msg;  /* heap-allocated on error, NULL otherwise */
} F64ArraysLlmResult;

/* llm_embedding — embed a single text string.
 * POSTs to {base_url}/embeddings.  Caller owns result.ok.data (free it when done). */
F64ArrayLlmResult  llm_embedding(TkLlmClient *client, const char *text);

/* llm_embeddings_batch — embed multiple texts in one request.
 * Caller owns result.data array and each element's .data pointer. */
F64ArraysLlmResult llm_embeddings_batch(TkLlmClient *client,
                                         const char *const *texts, uint64_t n);

/* llm_retry_backoff — configure exponential backoff for subsequent requests.
 * delay = base_delay_ms * 2^attempt, capped at 30 000 ms.
 * Calling this stores parameters in client; takes effect on the next request. */
void               llm_retry_backoff(TkLlmClient *client,
                                     uint64_t base_delay_ms, uint64_t max_retries);

/* -----------------------------------------------------------------------
 * Story 32.1.2 — JSON mode, usage tracking, vision
 * ----------------------------------------------------------------------- */

/* LlmUsage — cumulative token counts since client creation. */
typedef struct {
    uint64_t input_tokens;
    uint64_t output_tokens;
} LlmUsage;

/* llm_json_mode — like llm_chat but adds response_format:json_object (OpenAI)
 * or format:"json" (Ollama). */
TkLlmResp llm_json_mode(TkLlmClient *client, TkLlmMsg *messages, uint64_t n);

/* llm_usage — return cumulative token usage since client creation. */
LlmUsage  llm_usage(TkLlmClient *client);

/* llm_vision — send a messages array where a user message content beginning
 * with "http" or "data:" is treated as an image URL / data URI and wrapped
 * in the OpenAI vision content array format. */
TkLlmResp llm_vision(TkLlmClient *client, TkLlmMsg *messages, uint64_t n);

#endif /* TK_STDLIB_LLM_H */
