/*
 * test_llm_live.c — Live integration tests for std.llm (Story 17.1.3).
 *
 * These tests are SKIPPED automatically when the relevant API key env var is
 * not set.  They are NOT part of the default test suite; they are opt-in.
 *
 * Build and run: make test-stdlib-llm-live
 *
 * Required env vars (each test checks its own key and skips if absent):
 *   OPENAI_API_KEY    — tests against api.openai.com
 *   ANTHROPIC_API_KEY — tests against api.anthropic.com
 *   OLLAMA_HOST       — tests against local Ollama (e.g. http://localhost:11434)
 *
 * Example:
 *   OPENAI_API_KEY=sk-... make test-stdlib-llm-live
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "../../src/stdlib/llm.h"

static int failures = 0;
static int skipped  = 0;

#define SKIP(msg) \
    do { printf("SKIP: %s\n", (msg)); skipped++; return; } while (0)

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
         else { printf("pass: %s\n", (msg)); } } while (0)

#define ASSERT_NOTNULL(p, msg) ASSERT((p) != NULL, (msg))

/* ------------------------------------------------------------------ */

static void test_openai_chat(void)
{
    const char *key = getenv("OPENAI_API_KEY");
    if (!key || key[0] == '\0')
        SKIP("test_openai_chat (OPENAI_API_KEY not set)");

    TkLlmClient *c = llm_client("https://api.openai.com/v1", key, "gpt-4o-mini");
    ASSERT_NOTNULL(c, "openai_chat: llm_client not NULL");

    TkLlmMsg msgs[1];
    msgs[0].role    = "user";
    msgs[0].content = "Say 'ok' and nothing else";

    TkLlmResp resp = llm_chat(c, msgs, 1, 0.0);

    ASSERT(resp.is_err == 0,        "openai_chat: is_err == 0");
    ASSERT_NOTNULL(resp.content,    "openai_chat: content not NULL");
    ASSERT(resp.content[0] != '\0', "openai_chat: content not empty");

    llm_client_free(c);
}

/* ------------------------------------------------------------------ */

static void test_openai_stream(void)
{
    const char *key = getenv("OPENAI_API_KEY");
    if (!key || key[0] == '\0')
        SKIP("test_openai_stream (OPENAI_API_KEY not set)");

    TkLlmClient *c = llm_client("https://api.openai.com/v1", key, "gpt-4o-mini");
    ASSERT_NOTNULL(c, "openai_stream: llm_client not NULL");

    TkLlmMsg msgs[1];
    msgs[0].role    = "user";
    msgs[0].content = "Say 'ok' and nothing else";

    TkLlmStream s = llm_chatstream(c, msgs, 1, 0.0);

    ASSERT(s.is_err == 0, "openai_stream: is_err == 0");

    uint64_t    idx        = 0;
    uint64_t    chunk_count = 0;
    const char *chunk;
    while ((chunk = llm_streamnext(&s, &idx)) != NULL)
        chunk_count++;

    ASSERT(chunk_count >= 1, "openai_stream: at least 1 chunk returned");

    llm_client_free(c);
}

/* ------------------------------------------------------------------ */

static void test_anthropic_chat(void)
{
    const char *key = getenv("ANTHROPIC_API_KEY");
    if (!key || key[0] == '\0')
        SKIP("test_anthropic_chat (ANTHROPIC_API_KEY not set)");

    TkLlmClient *c = llm_client("https://api.anthropic.com/v1", key,
                                 "claude-haiku-4-5-20251001");
    ASSERT_NOTNULL(c, "anthropic_chat: llm_client not NULL");

    TkLlmMsg msgs[1];
    msgs[0].role    = "user";
    msgs[0].content = "Say 'ok' and nothing else";

    TkLlmResp resp = llm_chat(c, msgs, 1, 0.0);

    /*
     * HTTPS requires TLS support compiled in (see llm.h / llm.c).
     * If the response is an error mentioning TLS, skip rather than fail
     * so that builds without TLS do not break CI.
     */
    if (resp.is_err) {
        if (resp.err_msg &&
            (strstr(resp.err_msg, "TLS") || strstr(resp.err_msg, "tls") ||
             strstr(resp.err_msg, "SSL") || strstr(resp.err_msg, "ssl") ||
             strstr(resp.err_msg, "https") || strstr(resp.err_msg, "HTTPS"))) {
            printf("SKIP: test_anthropic_chat (TLS not supported in this build: %s)\n",
                   resp.err_msg);
            skipped++;
            llm_client_free(c);
            return;
        }
        fprintf(stderr, "FAIL: anthropic_chat: is_err != 0 (%s)\n",
                resp.err_msg ? resp.err_msg : "(no message)");
        failures++;
        llm_client_free(c);
        return;
    }

    ASSERT(resp.is_err == 0,        "anthropic_chat: is_err == 0");
    ASSERT_NOTNULL(resp.content,    "anthropic_chat: content not NULL");
    ASSERT(resp.content[0] != '\0', "anthropic_chat: content not empty");

    llm_client_free(c);
}

/* ------------------------------------------------------------------ */

static void test_ollama_chat(void)
{
    const char *host = getenv("OLLAMA_HOST");
    if (!host || host[0] == '\0')
        SKIP("test_ollama_chat (OLLAMA_HOST not set)");

    /* Ollama uses an OpenAI-compatible /v1 endpoint; no api_key required. */
    /* Append /v1 only if not already present in OLLAMA_HOST. */
    char base_url[512];
    if (strstr(host, "/v1"))
        snprintf(base_url, sizeof(base_url), "%s", host);
    else
        snprintf(base_url, sizeof(base_url), "%s/v1", host);

    TkLlmClient *c = llm_client(base_url, NULL, "llama3.2");
    ASSERT_NOTNULL(c, "ollama_chat: llm_client not NULL");

    TkLlmMsg msgs[1];
    msgs[0].role    = "user";
    msgs[0].content = "Reply with one word: hello";

    TkLlmResp resp = llm_chat(c, msgs, 1, 0.0);

    ASSERT(resp.is_err == 0,        "ollama_chat: is_err == 0");
    ASSERT_NOTNULL(resp.content,    "ollama_chat: content not NULL");
    ASSERT(resp.content[0] != '\0', "ollama_chat: content not empty");

    llm_client_free(c);
}

/* ------------------------------------------------------------------ */

static void test_token_count_estimate(void)
{
    /*
     * No network required.  llm_countokens is a pure computation
     * (strlen / 4) so any valid client pointer suffices; we construct
     * a lightweight one using whatever key is available, falling back
     * to a dummy client if none are set.
     */
    const char *key = getenv("OPENAI_API_KEY");
    if (!key || key[0] == '\0')
        key = getenv("ANTHROPIC_API_KEY");

    /* Use a local base URL so no real connection is attempted. */
    TkLlmClient *c = llm_client("http://localhost:11434/v1", key, "test-model");
    ASSERT_NOTNULL(c, "token_count: llm_client not NULL");

    uint64_t n = llm_countokens(c, "hello world this is a test");
    ASSERT(n > 0, "token_count: llm_countokens > 0 for non-empty string");

    llm_client_free(c);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_openai_chat();
    test_openai_stream();
    test_anthropic_chat();
    test_ollama_chat();
    test_token_count_estimate();

    printf("\nResults: %d passed, %d failed, %d skipped\n",
           (5 - failures - skipped), failures, skipped);
    return failures ? 1 : 0;
}
