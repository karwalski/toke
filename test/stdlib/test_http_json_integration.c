/*
 * test_http_json_integration.c — Integration test: HTTP API client workflow.
 *
 * Exercises the real-world pattern of:
 *   1. Build an HTTP request (via llm module's request builder)
 *   2. Receive a mock JSON API response
 *   3. Parse the JSON with json_dec / json_str / json_u64 / etc.
 *   4. Extract fields: string, number, nested object, array element
 *   5. Validate extracted data
 *   6. Handle errors: malformed JSON, missing fields
 *
 * Story: 21.1.1
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *      -I../../src/stdlib \
 *      test_http_json_integration.c \
 *      ../../src/stdlib/llm.c \
 *      ../../src/stdlib/json.c \
 *      ../../src/stdlib/http.c \
 *      ../../src/stdlib/str.c \
 *      -o test_http_json_integration
 */

#include "json.h"
#include "llm.h"
#include "http.h"
#include "str.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, label) \
    do { if (cond) { printf("PASS  %s\n", label); g_pass++; } \
         else      { printf("FAIL  %s\n", label); g_fail++; } } while(0)

/* ================================================================ */
/* Mock API response payloads                                        */
/* ================================================================ */

/* Realistic weather API response with nested objects and arrays. */
static const char *WEATHER_RESPONSE =
    "{"
        "\"status\":\"ok\","
        "\"code\":200,"
        "\"city\":\"Melbourne\","
        "\"temperature\":22.5,"
        "\"wind\":{\"speed\":15,\"direction\":\"NW\",\"gusts\":28},"
        "\"forecast\":["
            "{\"day\":\"Mon\",\"high\":24,\"low\":16},"
            "{\"day\":\"Tue\",\"high\":21,\"low\":14},"
            "{\"day\":\"Wed\",\"high\":19,\"low\":12}"
        "],"
        "\"alerts_active\":false"
    "}";

/* Realistic user/auth API response. */
static const char *USER_RESPONSE =
    "{"
        "\"id\":42,"
        "\"username\":\"karwalski\","
        "\"email\":\"matt@example.com\","
        "\"verified\":true,"
        "\"login_count\":1337,"
        "\"roles\":[\"admin\",\"editor\",\"viewer\"]"
    "}";

/* Malformed JSON payloads. */
static const char *MALFORMED_TRUNCATED = "{\"name\":\"alice\",\"age\":";
static const char *MALFORMED_NO_BRACE  = "just plain text, not JSON";
static const char *MALFORMED_BAD_ARRAY = "{\"items\":[1,2,}";

/* ================================================================ */
/* Test: Build HTTP request via llm_build_request                    */
/* ================================================================ */

static void test_build_request(void)
{
    printf("\n--- Build HTTP request body ---\n");

    TkLlmMsg msgs[2];
    msgs[0].role    = "system";
    msgs[0].content = "You are a weather assistant.";
    msgs[1].role    = "user";
    msgs[1].content = "What is the weather in Melbourne?";

    const char *body = llm_build_request(msgs, 2, "gpt-4o", 0.7, 0);
    CHECK(body != NULL, "llm_build_request returns non-NULL");

    /* The body should be valid JSON. */
    JsonResult jr = json_dec(body);
    CHECK(!jr.is_err, "llm_build_request output is valid JSON");

    /* Extract model field from the built request. */
    StrJsonResult model_r = json_str(jr.ok, "model");
    CHECK(!model_r.is_err && strcmp(model_r.ok, "gpt-4o") == 0,
          "request body contains model=gpt-4o");

    /* Should contain a messages array. */
    JsonArrayResult msgs_r = json_arr(jr.ok, "messages");
    CHECK(!msgs_r.is_err && msgs_r.ok.len == 2,
          "request body has messages array with 2 elements");

    /* First message should have role=system. */
    if (!msgs_r.is_err && msgs_r.ok.len >= 1) {
        StrJsonResult role_r = json_str(msgs_r.ok.data[0], "role");
        CHECK(!role_r.is_err && strcmp(role_r.ok, "system") == 0,
              "messages[0].role == system");
        free((void *)role_r.ok);
    }

    /* Second message should have role=user. */
    if (!msgs_r.is_err && msgs_r.ok.len >= 2) {
        StrJsonResult role_r = json_str(msgs_r.ok.data[1], "role");
        CHECK(!role_r.is_err && strcmp(role_r.ok, "user") == 0,
              "messages[1].role == user");
        free((void *)role_r.ok);

        StrJsonResult content_r = json_str(msgs_r.ok.data[1], "content");
        CHECK(!content_r.is_err &&
              strcmp(content_r.ok, "What is the weather in Melbourne?") == 0,
              "messages[1].content matches input");
        free((void *)content_r.ok);
    }

    /* Clean up array elements. */
    if (!msgs_r.is_err) {
        for (uint64_t i = 0; i < msgs_r.ok.len; i++)
            free((void *)msgs_r.ok.data[i].raw);
        free(msgs_r.ok.data);
    }
    free((void *)model_r.ok);
    free((void *)body);
}

/* ================================================================ */
/* Test: Build request with streaming flag                           */
/* ================================================================ */

static void test_build_request_stream(void)
{
    printf("\n--- Build HTTP request body (stream=true) ---\n");

    TkLlmMsg msgs[1];
    msgs[0].role    = "user";
    msgs[0].content = "Hello";

    const char *body = llm_build_request(msgs, 1, "claude-3", 1.0, 1);
    CHECK(body != NULL, "llm_build_request stream returns non-NULL");

    JsonResult jr = json_dec(body);
    CHECK(!jr.is_err, "stream request body is valid JSON");

    /* stream field should be true. */
    BoolJsonResult stream_r = json_bool(jr.ok, "stream");
    CHECK(!stream_r.is_err && stream_r.ok == 1,
          "stream request has stream=true");

    free((void *)body);
}

/* ================================================================ */
/* Test: Parse weather API response — full field extraction          */
/* ================================================================ */

static void test_parse_weather_response(void)
{
    printf("\n--- Parse weather API response ---\n");

    JsonResult jr = json_dec(WEATHER_RESPONSE);
    CHECK(!jr.is_err, "weather response parses successfully");

    Json j = jr.ok;

    /* String field. */
    StrJsonResult status_r = json_str(j, "status");
    CHECK(!status_r.is_err && strcmp(status_r.ok, "ok") == 0,
          "status == \"ok\"");
    free((void *)status_r.ok);

    StrJsonResult city_r = json_str(j, "city");
    CHECK(!city_r.is_err && strcmp(city_r.ok, "Melbourne") == 0,
          "city == \"Melbourne\"");
    free((void *)city_r.ok);

    /* Numeric field (u64). */
    U64JsonResult code_r = json_u64(j, "code");
    CHECK(!code_r.is_err && code_r.ok == 200, "code == 200");

    /* Float field. */
    F64JsonResult temp_r = json_f64(j, "temperature");
    CHECK(!temp_r.is_err && temp_r.ok > 22.4 && temp_r.ok < 22.6,
          "temperature ~= 22.5");

    /* Boolean field. */
    BoolJsonResult alerts_r = json_bool(j, "alerts_active");
    CHECK(!alerts_r.is_err && alerts_r.ok == 0,
          "alerts_active == false");

    /* Nested object: extract "wind" then drill into it. */
    /* json_str/json_u64 on a nested object isn't directly supported for dot
     * paths, but we can get the raw JSON of the nested object via json_arr
     * or by re-parsing.  Here we find the wind object raw value. */
    /* The json API works on top-level keys, so we use a trick:
     * extract the wind object's raw text and parse it as a new Json. */
    {
        /* json module does not have json_obj(), so we find wind via the raw
         * string.  We know the response is static, so we can locate it. */
        const char *wind_start = strstr(WEATHER_RESPONSE, "{\"speed\"");
        CHECK(wind_start != NULL, "located wind nested object in raw JSON");

        if (wind_start) {
            /* Build a new Json from the nested object substring.
             * json_dec validates the JSON so we can safely extract. */
            JsonResult wind_jr = json_dec(wind_start);
            CHECK(!wind_jr.is_err, "wind nested object parses");

            U64JsonResult speed_r = json_u64(wind_jr.ok, "speed");
            CHECK(!speed_r.is_err && speed_r.ok == 15, "wind.speed == 15");

            StrJsonResult dir_r = json_str(wind_jr.ok, "direction");
            CHECK(!dir_r.is_err && strcmp(dir_r.ok, "NW") == 0,
                  "wind.direction == \"NW\"");
            free((void *)dir_r.ok);

            U64JsonResult gusts_r = json_u64(wind_jr.ok, "gusts");
            CHECK(!gusts_r.is_err && gusts_r.ok == 28,
                  "wind.gusts == 28");
        }
    }

    /* Array: extract forecast, iterate elements. */
    JsonArrayResult fc_r = json_arr(j, "forecast");
    CHECK(!fc_r.is_err && fc_r.ok.len == 3, "forecast array has 3 elements");

    if (!fc_r.is_err && fc_r.ok.len == 3) {
        /* First forecast entry. */
        StrJsonResult day0_r = json_str(fc_r.ok.data[0], "day");
        CHECK(!day0_r.is_err && strcmp(day0_r.ok, "Mon") == 0,
              "forecast[0].day == \"Mon\"");
        free((void *)day0_r.ok);

        U64JsonResult high0_r = json_u64(fc_r.ok.data[0], "high");
        CHECK(!high0_r.is_err && high0_r.ok == 24,
              "forecast[0].high == 24");

        U64JsonResult low0_r = json_u64(fc_r.ok.data[0], "low");
        CHECK(!low0_r.is_err && low0_r.ok == 16,
              "forecast[0].low == 16");

        /* Last forecast entry. */
        StrJsonResult day2_r = json_str(fc_r.ok.data[2], "day");
        CHECK(!day2_r.is_err && strcmp(day2_r.ok, "Wed") == 0,
              "forecast[2].day == \"Wed\"");
        free((void *)day2_r.ok);

        U64JsonResult high2_r = json_u64(fc_r.ok.data[2], "high");
        CHECK(!high2_r.is_err && high2_r.ok == 19,
              "forecast[2].high == 19");

        /* Clean up array elements. */
        for (uint64_t i = 0; i < fc_r.ok.len; i++)
            free((void *)fc_r.ok.data[i].raw);
        free(fc_r.ok.data);
    }
}

/* ================================================================ */
/* Test: Parse user API response — verified, roles array             */
/* ================================================================ */

static void test_parse_user_response(void)
{
    printf("\n--- Parse user API response ---\n");

    JsonResult jr = json_dec(USER_RESPONSE);
    CHECK(!jr.is_err, "user response parses successfully");

    Json j = jr.ok;

    U64JsonResult id_r = json_u64(j, "id");
    CHECK(!id_r.is_err && id_r.ok == 42, "id == 42");

    StrJsonResult user_r = json_str(j, "username");
    CHECK(!user_r.is_err && strcmp(user_r.ok, "karwalski") == 0,
          "username == \"karwalski\"");
    free((void *)user_r.ok);

    StrJsonResult email_r = json_str(j, "email");
    CHECK(!email_r.is_err && strcmp(email_r.ok, "matt@example.com") == 0,
          "email == \"matt@example.com\"");
    free((void *)email_r.ok);

    BoolJsonResult ver_r = json_bool(j, "verified");
    CHECK(!ver_r.is_err && ver_r.ok == 1, "verified == true");

    U64JsonResult lc_r = json_u64(j, "login_count");
    CHECK(!lc_r.is_err && lc_r.ok == 1337, "login_count == 1337");

    /* Roles array — extract and check first element. */
    JsonArrayResult roles_r = json_arr(j, "roles");
    CHECK(!roles_r.is_err && roles_r.ok.len == 3,
          "roles array has 3 elements");

    if (!roles_r.is_err && roles_r.ok.len == 3) {
        /* Each element is a raw JSON string literal like "\"admin\"".
         * We parse via json_dec wrapping it in a trivial object. */
        /* Actually, array elements are raw JSON values. A bare string
         * "\"admin\"" won't json_dec (needs object/array).  We can check
         * the raw directly. */
        CHECK(strcmp(roles_r.ok.data[0].raw, "\"admin\"") == 0,
              "roles[0] raw == \"admin\"");
        CHECK(strcmp(roles_r.ok.data[1].raw, "\"editor\"") == 0,
              "roles[1] raw == \"editor\"");
        CHECK(strcmp(roles_r.ok.data[2].raw, "\"viewer\"") == 0,
              "roles[2] raw == \"viewer\"");

        for (uint64_t i = 0; i < roles_r.ok.len; i++)
            free((void *)roles_r.ok.data[i].raw);
        free(roles_r.ok.data);
    }
}

/* ================================================================ */
/* Test: Error handling — malformed JSON                              */
/* ================================================================ */

static void test_malformed_json(void)
{
    printf("\n--- Malformed JSON error handling ---\n");

    /* Truncated JSON. */
    JsonResult tr = json_dec(MALFORMED_TRUNCATED);
    CHECK(tr.is_err && tr.err.kind == JSON_ERR_PARSE,
          "truncated JSON returns parse error");

    /* Plain text (no opening brace/bracket). */
    JsonResult pr = json_dec(MALFORMED_NO_BRACE);
    CHECK(pr.is_err && pr.err.kind == JSON_ERR_PARSE,
          "plain text returns parse error");

    /* Bad array syntax. */
    JsonResult ar = json_dec(MALFORMED_BAD_ARRAY);
    CHECK(ar.is_err && ar.err.kind == JSON_ERR_PARSE,
          "bad array syntax returns parse error");

    /* NULL input to json_dec. */
    /* json_dec dereferences s directly so we skip NULL to avoid crash;
     * this is documented behaviour — callers validate input. */

    /* Empty object — valid but has no fields. */
    JsonResult er = json_dec("{}");
    CHECK(!er.is_err, "empty object parses successfully");

    StrJsonResult missing_r = json_str(er.ok, "anything");
    CHECK(missing_r.is_err && missing_r.err.kind == JSON_ERR_MISSING,
          "missing field on empty object returns MISSING error");
}

/* ================================================================ */
/* Test: Type mismatch errors                                        */
/* ================================================================ */

static void test_type_mismatch(void)
{
    printf("\n--- Type mismatch error handling ---\n");

    JsonResult jr = json_dec("{\"name\":\"alice\",\"age\":30,\"active\":true}");
    CHECK(!jr.is_err, "type mismatch test object parses");

    Json j = jr.ok;

    /* Try to read string as u64. */
    U64JsonResult u_r = json_u64(j, "name");
    CHECK(u_r.is_err && u_r.err.kind == JSON_ERR_TYPE,
          "string read as u64 returns TYPE error");

    /* Try to read number as bool. */
    BoolJsonResult b_r = json_bool(j, "age");
    CHECK(b_r.is_err && b_r.err.kind == JSON_ERR_TYPE,
          "number read as bool returns TYPE error");

    /* Try to read string as bool. */
    BoolJsonResult b2_r = json_bool(j, "name");
    CHECK(b2_r.is_err && b2_r.err.kind == JSON_ERR_TYPE,
          "string read as bool returns TYPE error");

    /* Try to read number as array. */
    JsonArrayResult a_r = json_arr(j, "age");
    CHECK(a_r.is_err && a_r.err.kind == JSON_ERR_TYPE,
          "number read as array returns TYPE error");
}

/* ================================================================ */
/* Test: End-to-end — build request, mock response, extract          */
/* ================================================================ */

static void test_end_to_end_workflow(void)
{
    printf("\n--- End-to-end: request build -> response parse ---\n");

    /* Step 1: Build the request. */
    TkLlmMsg msgs[1];
    msgs[0].role    = "user";
    msgs[0].content = "Get user profile for id=42";

    const char *req_body = llm_build_request(msgs, 1, "gpt-4o", 0.0, 0);
    CHECK(req_body != NULL, "e2e: request body built");

    /* Verify it is valid JSON. */
    JsonResult req_jr = json_dec(req_body);
    CHECK(!req_jr.is_err, "e2e: request body is valid JSON");

    /* Step 2: Simulate receiving the API response (USER_RESPONSE). */
    /* In a real system, http_post would send req_body and get back a
     * response.  Here we use the mock response directly. */

    /* Step 3: Parse the response. */
    JsonResult resp_jr = json_dec(USER_RESPONSE);
    CHECK(!resp_jr.is_err, "e2e: response parses");

    /* Step 4: Extract and validate specific fields. */
    U64JsonResult id_r = json_u64(resp_jr.ok, "id");
    CHECK(!id_r.is_err && id_r.ok == 42, "e2e: extracted id == 42");

    StrJsonResult user_r = json_str(resp_jr.ok, "username");
    CHECK(!user_r.is_err && strcmp(user_r.ok, "karwalski") == 0,
          "e2e: extracted username");

    BoolJsonResult ver_r = json_bool(resp_jr.ok, "verified");
    CHECK(!ver_r.is_err && ver_r.ok == 1, "e2e: user is verified");

    free((void *)user_r.ok);
    free((void *)req_body);
}

/* ================================================================ */
/* Test: HTTP types — construct Req/Res manually                     */
/* ================================================================ */

static void test_http_types(void)
{
    printf("\n--- HTTP types construction ---\n");

    /* Construct an HTTP request struct to verify types work. */
    StrPair hdrs[2];
    hdrs[0].key = "Content-Type";  hdrs[0].val = "application/json";
    hdrs[1].key = "Authorization"; hdrs[1].val = "Bearer test-key";

    StrPair params[1];
    params[0].key = "id"; params[0].val = "42";

    Req req;
    req.method      = "POST";
    req.path        = "/v1/chat/completions";
    req.body        = "{\"model\":\"gpt-4o\"}";
    req.headers.data = hdrs;
    req.headers.len  = 2;
    req.params.data  = params;
    req.params.len   = 1;

    CHECK(strcmp(req.method, "POST") == 0, "Req.method == POST");
    CHECK(strcmp(req.path, "/v1/chat/completions") == 0, "Req.path correct");
    CHECK(req.headers.len == 2, "Req has 2 headers");
    CHECK(strcmp(req.headers.data[0].val, "application/json") == 0,
          "Req Content-Type header set");

    /* Construct a response. */
    Res res = http_Res_json(200, USER_RESPONSE);
    CHECK(res.status == 200, "Res.status == 200");
    CHECK(res.body != NULL, "Res.body is not NULL");

    /* Parse the response body as JSON. */
    JsonResult jr = json_dec(res.body);
    CHECK(!jr.is_err, "Res.body is valid JSON");

    StrJsonResult user_r = json_str(jr.ok, "username");
    CHECK(!user_r.is_err && strcmp(user_r.ok, "karwalski") == 0,
          "parsed Res.body username == karwalski");
    free((void *)user_r.ok);
}

/* ================================================================ */
/* Test: llm_parse_sse_chunk with JSON delta                         */
/* ================================================================ */

static void test_sse_chunk_parsing(void)
{
    printf("\n--- SSE chunk parsing ---\n");

    /* Normal delta chunk from a streaming response. */
    const char *chunk_json =
        "{\"choices\":[{\"delta\":{\"content\":\"Hello world\"}}]}";

    const char *content = llm_parse_sse_chunk(chunk_json);
    CHECK(content != NULL && strcmp(content, "Hello world") == 0,
          "sse chunk extracts delta content");
    free((void *)content);

    /* [DONE] sentinel. */
    const char *done = llm_parse_sse_chunk("[DONE]");
    CHECK(done == NULL, "sse [DONE] returns NULL");

    /* Empty delta content. */
    const char *empty_chunk =
        "{\"choices\":[{\"delta\":{\"content\":\"\"}}]}";
    const char *empty_content = llm_parse_sse_chunk(empty_chunk);
    CHECK(empty_content != NULL && strcmp(empty_content, "") == 0,
          "sse empty delta returns empty string");
    free((void *)empty_content);
}

/* ================================================================ */
/* main                                                              */
/* ================================================================ */

int main(void)
{
    printf("=== HTTP + JSON Integration Tests (Story 21.1.1) ===\n");

    test_build_request();
    test_build_request_stream();
    test_parse_weather_response();
    test_parse_user_response();
    test_malformed_json();
    test_type_mismatch();
    test_end_to_end_workflow();
    test_http_types();
    test_sse_chunk_parsing();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
