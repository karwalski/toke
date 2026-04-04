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

#endif /* TK_STDLIB_LLM_H */
