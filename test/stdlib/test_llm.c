/*
 * test_llm.c — Unit tests for the std.llm C library (Story 17.1.1).
 *
 * Tests cover helper functions only; no real network calls are made.
 *
 * Build and run: make test-stdlib-llm
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../src/stdlib/llm.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

#define ASSERT_NULL(p, msg) \
    ASSERT((p) == NULL, msg)

#define ASSERT_NOTNULL(p, msg) \
    ASSERT((p) != NULL, msg)

/* helper: returns 1 if haystack contains needle */
static int contains(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    return strstr(haystack, needle) != NULL;
}

int main(void)
{
    /* ------------------------------------------------------------------ */
    /* llm_build_request tests                                             */
    /* ------------------------------------------------------------------ */

    /* Test 1: single user message contains required keys */
    {
        TkLlmMsg msgs[1];
        msgs[0].role    = "user";
        msgs[0].content = "hello";
        const char *req = llm_build_request(msgs, 1, "gpt-4o", 0.7, 0);
        ASSERT_NOTNULL(req, "build_request: returns non-null");
        ASSERT(contains(req, "\"messages\""),   "build_request: contains messages key");
        ASSERT(contains(req, "\"model\""),       "build_request: contains model key");
        ASSERT(contains(req, "\"role\":\"user\""),    "build_request: contains role:user");
        ASSERT(contains(req, "\"content\":\"hello\""), "build_request: contains content:hello");
        ASSERT(contains(req, "\"temperature\""), "build_request: contains temperature key");
        free((void *)req);
    }

    /* Test 6: stream=1 produces "stream":true */
    {
        TkLlmMsg msgs[1];
        msgs[0].role    = "user";
        msgs[0].content = "ping";
        const char *req = llm_build_request(msgs, 1, "gpt-4o", 0.5, 1);
        ASSERT_NOTNULL(req, "build_request stream=1: non-null");
        ASSERT(contains(req, "\"stream\":true"), "build_request stream=1: contains stream:true");
        free((void *)req);
    }

    /* Test 7: stream=0 produces "stream":false */
    {
        TkLlmMsg msgs[1];
        msgs[0].role    = "user";
        msgs[0].content = "ping";
        const char *req = llm_build_request(msgs, 1, "gpt-4o", 0.5, 0);
        ASSERT_NOTNULL(req, "build_request stream=0: non-null");
        ASSERT(contains(req, "\"stream\":false"), "build_request stream=0: contains stream:false");
        free((void *)req);
    }

    /* ------------------------------------------------------------------ */
    /* llm_parse_sse_chunk tests                                           */
    /* ------------------------------------------------------------------ */

    /* Test 8: normal delta with content */
    {
        const char *sse = "{\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}";
        const char *got = llm_parse_sse_chunk(sse);
        ASSERT_NOTNULL(got, "parse_sse_chunk: non-null for normal delta");
        ASSERT_STREQ(got, "hello", "parse_sse_chunk: extracts content 'hello'");
        free((void *)got);
    }

    /* Test 9: delta with no content key (e.g. role-only chunk) */
    {
        const char *sse = "{\"choices\":[{\"delta\":{}}]}";
        const char *got = llm_parse_sse_chunk(sse);
        ASSERT_NOTNULL(got, "parse_sse_chunk: non-null for empty delta");
        ASSERT(strlen(got) == 0, "parse_sse_chunk: empty string for delta without content");
        free((void *)got);
    }

    /* Test 10: [DONE] sentinel returns NULL */
    {
        const char *got = llm_parse_sse_chunk("[DONE]");
        ASSERT_NULL(got, "parse_sse_chunk: NULL for [DONE]");
    }

    /* ------------------------------------------------------------------ */
    /* llm_countokens tests                                                */
    /* ------------------------------------------------------------------ */

    /* Test 11: "hello world" = 11 chars / 4 = 2 */
    {
        TkLlmClient *c = llm_client("http://localhost:11434/v1", NULL, "llama3");
        ASSERT_NOTNULL(c, "llm_client: non-null");
        uint64_t tok = llm_countokens(c, "hello world");
        ASSERT(tok == 2, "countokens: 'hello world' -> 2");
        llm_client_free(c);
    }

    /* Test 12: empty string = 0 */
    {
        TkLlmClient *c = llm_client("http://localhost:11434/v1", NULL, "llama3");
        uint64_t tok = llm_countokens(c, "");
        ASSERT(tok == 0, "countokens: empty string -> 0");
        llm_client_free(c);
    }

    /* ------------------------------------------------------------------ */
    /* llm_streamnext tests                                                */
    /* ------------------------------------------------------------------ */

    /* Test 13-16: 3-chunk stream returns each in order, then NULL */
    {
        const char *chunk_data[] = { "foo", "bar", "baz", NULL };
        TkLlmStream s;
        s.chunks  = chunk_data;
        s.nchunks = 3;
        s.is_err  = 0;
        s.err_msg = NULL;

        uint64_t idx = 0;
        const char *c0 = llm_streamnext(&s, &idx);
        ASSERT_STREQ(c0, "foo", "streamnext: first chunk is 'foo'");
        const char *c1 = llm_streamnext(&s, &idx);
        ASSERT_STREQ(c1, "bar", "streamnext: second chunk is 'bar'");
        const char *c2 = llm_streamnext(&s, &idx);
        ASSERT_STREQ(c2, "baz", "streamnext: third chunk is 'baz'");
        const char *c3 = llm_streamnext(&s, &idx);
        ASSERT_NULL(c3, "streamnext: returns NULL after last chunk");
    }

    /* ------------------------------------------------------------------ */
    /* llm_client struct field tests                                       */
    /* ------------------------------------------------------------------ */

    /* Test 17: llm_client populates fields correctly */
    {
        TkLlmClient *c = llm_client("http://localhost:11434/v1", "sk-test", "mistral");
        ASSERT_NOTNULL(c, "llm_client: non-null result");
        ASSERT_STREQ(c->base_url, "http://localhost:11434/v1", "llm_client: base_url matches");
        ASSERT_STREQ(c->api_key,  "sk-test",                  "llm_client: api_key matches");
        ASSERT_STREQ(c->model,    "mistral",                   "llm_client: model matches");
        ASSERT(c->timeout_ms  == 0, "llm_client: timeout_ms default 0");
        ASSERT(c->max_retries == 0, "llm_client: max_retries default 0");
        llm_client_free(c);
    }

    /* Test 18: llm_client with NULL api_key (local inference) */
    {
        TkLlmClient *c = llm_client("http://localhost:11434/v1", NULL, "llama3");
        ASSERT_NOTNULL(c, "llm_client NULL key: non-null result");
        ASSERT_NULL(c->api_key, "llm_client NULL key: api_key is NULL");
        llm_client_free(c);
    }

    /* ------------------------------------------------------------------ */
    /* build_request: multi-message conversation                           */
    /* ------------------------------------------------------------------ */

    /* Test 19: two messages both appear in output */
    {
        TkLlmMsg msgs[2];
        msgs[0].role    = "system";
        msgs[0].content = "You are helpful.";
        msgs[1].role    = "user";
        msgs[1].content = "What is 2+2?";
        const char *req = llm_build_request(msgs, 2, "gpt-4o-mini", 0.0, 0);
        ASSERT_NOTNULL(req, "build_request multi-msg: non-null");
        ASSERT(contains(req, "\"role\":\"system\""), "build_request multi-msg: contains system role");
        ASSERT(contains(req, "\"role\":\"user\""),   "build_request multi-msg: contains user role");
        ASSERT(contains(req, "What is 2+2?"),        "build_request multi-msg: contains user content");
        free((void *)req);
    }

    /* ------------------------------------------------------------------ */
    /* parse_sse_chunk: JSON-escaped content                               */
    /* ------------------------------------------------------------------ */

    /* Test 20: content with escaped newline round-trips correctly */
    {
        const char *sse = "{\"choices\":[{\"delta\":{\"content\":\"line1\\nline2\"}}]}";
        const char *got = llm_parse_sse_chunk(sse);
        ASSERT_NOTNULL(got, "parse_sse_chunk escaped: non-null");
        ASSERT(contains(got, "line1"), "parse_sse_chunk escaped: contains line1");
        free((void *)got);
    }

    /* ------------------------------------------------------------------ */
    /* Story 32.1.1 — llm_retry_backoff                                   */
    /* ------------------------------------------------------------------ */

    /* Test 21: llm_retry_backoff stores values in client struct */
    {
        TkLlmClient *c = llm_client("http://localhost:11434/v1", NULL, "llama3");
        ASSERT_NOTNULL(c, "retry_backoff: client non-null");
        /* fields start at 0 */
        ASSERT(c->retry_base_delay_ms == 0, "retry_backoff: base_delay_ms default 0");
        ASSERT(c->retry_max_retries   == 0, "retry_backoff: max_retries default 0");
        /* after call, fields are updated */
        llm_retry_backoff(c, 200, 5);
        ASSERT(c->retry_base_delay_ms == 200, "retry_backoff: base_delay_ms set to 200");
        ASSERT(c->retry_max_retries   == 5,   "retry_backoff: max_retries set to 5");
        llm_client_free(c);
    }

    /* Test 22: llm_retry_backoff with NULL client does not crash */
    {
        llm_retry_backoff(NULL, 100, 3); /* must not crash */
        ASSERT(1, "retry_backoff: NULL client safe (no crash)");
    }

    /* ------------------------------------------------------------------ */
    /* Story 32.1.2 — llm_usage                                           */
    /* ------------------------------------------------------------------ */

    /* Test 23: fresh client has zero usage */
    {
        TkLlmClient *c = llm_client("http://localhost:11434/v1", NULL, "llama3");
        ASSERT_NOTNULL(c, "llm_usage: client non-null");
        LlmUsage u = llm_usage(c);
        ASSERT(u.input_tokens  == 0, "llm_usage: fresh client input_tokens == 0");
        ASSERT(u.output_tokens == 0, "llm_usage: fresh client output_tokens == 0");
        llm_client_free(c);
    }

    /* Test 24: llm_usage with NULL client returns zeros */
    {
        LlmUsage u = llm_usage(NULL);
        ASSERT(u.input_tokens  == 0, "llm_usage: NULL client input_tokens == 0");
        ASSERT(u.output_tokens == 0, "llm_usage: NULL client output_tokens == 0");
    }

    /* ------------------------------------------------------------------ */
    /* Story 32.1.1 — llm_embedding: offline / empty-URL error path       */
    /* ------------------------------------------------------------------ */

    /* Test 25: llm_embedding with empty base_url returns is_err=1 (not crash) */
    {
        TkLlmClient *c = llm_client("", NULL, "text-embedding-3-small");
        ASSERT_NOTNULL(c, "llm_embedding offline: client non-null");
        F64ArrayLlmResult r = llm_embedding(c, "hello");
        ASSERT(r.is_err == 1, "llm_embedding offline: is_err == 1 for invalid URL");
        if (r.err_msg) free((void *)r.err_msg);
        llm_client_free(c);
    }

    /* Test 26: llm_embeddings_batch with empty base_url returns is_err=1 */
    {
        TkLlmClient *c = llm_client("", NULL, "text-embedding-3-small");
        ASSERT_NOTNULL(c, "llm_embeddings_batch offline: client non-null");
        const char *texts[] = { "alpha", "beta" };
        F64ArraysLlmResult r = llm_embeddings_batch(c, texts, 2);
        ASSERT(r.is_err == 1, "llm_embeddings_batch offline: is_err == 1 for invalid URL");
        if (r.err_msg) free((void *)r.err_msg);
        llm_client_free(c);
    }

    /* Test 27: llm_embedding with NULL client returns is_err=1 */
    {
        F64ArrayLlmResult r = llm_embedding(NULL, "hello");
        ASSERT(r.is_err == 1, "llm_embedding: NULL client -> is_err == 1");
        if (r.err_msg) free((void *)r.err_msg);
    }

    /* ------------------------------------------------------------------ */
    /* Story 32.1.2 — llm_json_mode: offline / empty-URL error path       */
    /* ------------------------------------------------------------------ */

    /* Test 28: llm_json_mode with empty base_url returns is_err=1 */
    {
        TkLlmClient *c = llm_client("", NULL, "gpt-4o");
        ASSERT_NOTNULL(c, "llm_json_mode offline: client non-null");
        TkLlmMsg msg;
        msg.role    = "user";
        msg.content = "Return JSON";
        TkLlmResp r = llm_json_mode(c, &msg, 1);
        ASSERT(r.is_err == 1, "llm_json_mode offline: is_err == 1 for invalid URL");
        if (r.err_msg)  free((void *)r.err_msg);
        if (r.content)  free((void *)r.content);
        llm_client_free(c);
    }

    /* Test 29: llm_json_mode with NULL client returns is_err=1 */
    {
        TkLlmMsg msg;
        msg.role    = "user";
        msg.content = "{}";
        TkLlmResp r = llm_json_mode(NULL, &msg, 1);
        ASSERT(r.is_err == 1, "llm_json_mode: NULL client -> is_err == 1");
        if (r.err_msg) free((void *)r.err_msg);
        if (r.content) free((void *)r.content);
    }

    /* ------------------------------------------------------------------ */
    /* Story 32.1.2 — llm_vision: offline / empty-URL error path          */
    /* ------------------------------------------------------------------ */

    /* Test 30: llm_vision with empty base_url returns is_err=1 */
    {
        TkLlmClient *c = llm_client("", NULL, "gpt-4o");
        ASSERT_NOTNULL(c, "llm_vision offline: client non-null");
        TkLlmMsg msg;
        msg.role    = "user";
        msg.content = "https://example.com/image.jpg";
        TkLlmResp r = llm_vision(c, &msg, 1);
        ASSERT(r.is_err == 1, "llm_vision offline: is_err == 1 for invalid URL");
        if (r.err_msg)  free((void *)r.err_msg);
        if (r.content)  free((void *)r.content);
        llm_client_free(c);
    }

    /* Test 31: llm_vision with NULL client returns is_err=1 */
    {
        TkLlmMsg msg;
        msg.role    = "user";
        msg.content = "https://example.com/image.jpg";
        TkLlmResp r = llm_vision(NULL, &msg, 1);
        ASSERT(r.is_err == 1, "llm_vision: NULL client -> is_err == 1");
        if (r.err_msg) free((void *)r.err_msg);
        if (r.content) free((void *)r.content);
    }

    /* ------------------------------------------------------------------ */
    /* Summary                                                             */
    /* ------------------------------------------------------------------ */

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}
