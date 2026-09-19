/*
 * llm_glue.c — i64-ABI wrappers for std.llm.
 *
 * Extracted from tk_web_glue.c (story 136.18), the same move toon_glue.c
 * (114.35) and net_glue.c (114.31) made: these wrappers only ever linked when
 * std.http dragged tk_web_glue.c in, so a standalone `i=llm:std.llm` program
 * died at link with E9003 naming tk_llm_chat_w. src/stdlib_deps.c now lists
 * this file under the llm module.
 */
#include "llm.h"
#include "tk_array.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 114.53/114.54/127.67: a wrapper reports failure through this flag and still
 * returns the real value; defined in tk_runtime.c. */
extern int64_t tk_current_error;

/*
 * Story 136.18 — llm.client / llm.chat / llm.complete take the client.
 *
 * stdlib/llm.tki, docs/stdlib/llm.md and llm.h all declare
 * llm.chat(llmclient; @(llmmsg)) and llm.complete(llmclient; str), and
 * llm_chat/llm_complete in llm.c have always taken a TkLlmClient *.  The
 * wrappers below took only the payload and called llm_ensure_client(), a
 * lazily-created process-wide singleton built from $LLM_BASE_URL /
 * $LLM_API_KEY / $LLM_MODEL.  The client a caller constructed was not merely
 * ignored -- llm.client had no wrapper at all, so there was no way to build
 * one.  Two endpoints in one process, or a key that differs per request, were
 * impossible rather than awkward, and every call silently went wherever the
 * environment pointed.
 *
 * g_llm_client / llm_ensure_client are gone with them: an implicit global is
 * what the story is about, and leaving it would leave the trap.
 */
int64_t tk_llm_client_w(int64_t base_url, int64_t api_key, int64_t model) {
    const char *u = base_url ? (const char *)(intptr_t)base_url : NULL;
    const char *k = api_key  ? (const char *)(intptr_t)api_key  : NULL;
    const char *m = model    ? (const char *)(intptr_t)model    : NULL;
    if (!u || !*u) return 0;
    return (int64_t)(intptr_t)llm_client(u, k, m && *m ? m : "llama3");
}

/*
 * decode_llm_msgs — a toke @(llmmsg) into a TkLlmMsg array.
 *
 * Each element is a pointer to a 2-slot i64 struct block { role, content },
 * which is how the compiler materialises a two-field struct.  Length comes
 * from the array header (handle[-1]), not from a guess about plausible
 * counts: the old wrapper decided "is this an array or a bare string?" by
 * testing whether handle[-1] fell between 1 and 100, which reads memory in
 * front of a string as a length.
 */
static TkLlmMsg *decode_llm_msgs(int64_t messages, uint64_t *out_n) {
    *out_n = 0;
    if (!messages) return NULL;
    int64_t n = tk_arr_len(messages);
    if (n <= 0) return NULL;
    int64_t *ptr = (int64_t *)(intptr_t)messages;
    TkLlmMsg *msgs = (TkLlmMsg *)malloc((size_t)n * sizeof(TkLlmMsg));
    if (!msgs) return NULL;
    for (int64_t i = 0; i < n; i++) {
        int64_t *sp = (int64_t *)(intptr_t)ptr[i];
        msgs[i].role    = sp ? (const char *)(intptr_t)sp[0] : "user";
        msgs[i].content = sp ? (const char *)(intptr_t)sp[1] : "";
    }
    *out_n = (uint64_t)n;
    return msgs;
}

/*
 * llmresp_block — a $llmresp as the compiler lays it out: one i64 slot per
 * field, { content:str, tokensin:u64, tokensout:u64, model:str }.  Handing
 * back the raw C TkLlmResp instead would have the consumer read is_err as
 * the model name (127.86).
 */
static int64_t llmresp_block(const char *content, uint64_t tin, uint64_t tout,
                             const char *model) {
    int64_t *b = (int64_t *)malloc(4 * sizeof(int64_t));
    if (!b) return 0;
    b[0] = (int64_t)(intptr_t)(content ? content : "");
    b[1] = (int64_t)tin;
    b[2] = (int64_t)tout;
    b[3] = (int64_t)(intptr_t)(model ? model : "");
    return (int64_t)(intptr_t)b;
}

int64_t tk_llm_complete_w(int64_t client, int64_t prompt) {
    if (!client || !prompt) { tk_current_error = 1; return 0; }
    TkLlmClient *c = (TkLlmClient *)(intptr_t)client;
    TkLlmResp r = llm_complete(c, (const char *)(intptr_t)prompt, 0.7);
    if (r.is_err || !r.content) { tk_current_error = 1; return 0; }
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.content;
}

int64_t tk_llm_chat_w(int64_t client, int64_t messages) {
    if (!client) { tk_current_error = 1; return 0; }
    TkLlmClient *c = (TkLlmClient *)(intptr_t)client;
    uint64_t n = 0;
    TkLlmMsg *msgs = decode_llm_msgs(messages, &n);
    if (!msgs) { tk_current_error = 1; return 0; }
    TkLlmResp r = llm_chat(c, msgs, n, 0.7);
    free(msgs);
    if (r.is_err || !r.content) { tk_current_error = 1; return 0; }
    tk_current_error = 0;
    return llmresp_block(r.content, r.input_tokens, r.output_tokens, c->model);
}

int64_t tk_llm_countokens_w(int64_t client, int64_t text) {
    if (!client || !text) return 0;
    TkLlmClient *c = (TkLlmClient *)(intptr_t)client;
    return (int64_t)llm_countokens(c, (const char *)(intptr_t)text);
}
