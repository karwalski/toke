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
