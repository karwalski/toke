/*
 * test_llm_tool.c — Unit tests for the std.llm.tool C library (Story 17.1.2).
 *
 * Build and run: make test-stdlib-llm-tool
 *
 * Tests cover JSON builders and parsers only; no network calls are made.
 *
 * Story: 17.1.2
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/llm_tool.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack),(needle)) != NULL, msg)

#define ASSERT_NOT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack),(needle)) == NULL, msg)

/* -----------------------------------------------------------------------
 * Fixtures
 * ----------------------------------------------------------------------- */

/* Minimal OpenAI-style response with one tool call */
static const char *RESPONSE_ONE_TOOL_CALL =
    "{"
    "  \"id\": \"chatcmpl-abc123\","
    "  \"choices\": [{"
    "    \"message\": {"
    "      \"role\": \"assistant\","
    "      \"tool_calls\": ["
    "        {"
    "          \"id\": \"call_xyz999\","
    "          \"type\": \"function\","
    "          \"function\": {"
    "            \"name\": \"get_weather\","
    "            \"arguments\": \"{\\\"location\\\":\\\"Sydney\\\",\\\"unit\\\":\\\"celsius\\\"}\""
    "          }"
    "        }"
    "      ]"
    "    }"
    "  }]"
    "}";

/* Response with two tool calls */
static const char *RESPONSE_TWO_TOOL_CALLS =
    "{"
    "  \"choices\": [{"
    "    \"message\": {"
    "      \"tool_calls\": ["
    "        {"
    "          \"id\": \"call_aaa\","
    "          \"type\": \"function\","
    "          \"function\": {"
    "            \"name\": \"tool_one\","
    "            \"arguments\": \"{\\\"x\\\":1}\""
    "          }"
    "        },"
    "        {"
    "          \"id\": \"call_bbb\","
    "          \"type\": \"function\","
    "          \"function\": {"
    "            \"name\": \"tool_two\","
    "            \"arguments\": \"{\\\"y\\\":2}\""
    "          }"
    "        }"
    "      ]"
    "    }"
    "  }]"
    "}";

/* Response with no tool calls (plain text reply) */
static const char *RESPONSE_NO_TOOL_CALLS =
    "{"
    "  \"choices\": [{"
    "    \"message\": {"
    "      \"role\": \"assistant\","
    "      \"content\": \"Sure, here is your answer.\""
    "    }"
    "  }]"
    "}";

/* -----------------------------------------------------------------------
 * Tests: llm_tool_build_tools_json — single tool, no params
 * ----------------------------------------------------------------------- */

static void test_build_tools_json_single_tool_has_type_function(void)
{
    TkToolDecl tool;
    tool.name        = "get_weather";
    tool.description = "Get the current weather";
    tool.params      = NULL;
    tool.nparams     = 0;

    const char *json = llm_tool_build_tools_json(&tool, 1);
    ASSERT_CONTAINS(json, "\"type\":\"function\"",
                    "build_tools_json single tool: contains type:function");
    free((char *)json);
}

static void test_build_tools_json_single_tool_has_name(void)
{
    TkToolDecl tool;
    tool.name        = "get_weather";
    tool.description = "Get the current weather";
    tool.params      = NULL;
    tool.nparams     = 0;

    const char *json = llm_tool_build_tools_json(&tool, 1);
    ASSERT_CONTAINS(json, "get_weather",
                    "build_tools_json single tool: contains tool name");
    free((char *)json);
}

static void test_build_tools_json_single_tool_has_description(void)
{
    TkToolDecl tool;
    tool.name        = "get_weather";
    tool.description = "Get the current weather";
    tool.params      = NULL;
    tool.nparams     = 0;

    const char *json = llm_tool_build_tools_json(&tool, 1);
    ASSERT_CONTAINS(json, "Get the current weather",
                    "build_tools_json single tool: contains description");
    free((char *)json);
}

/* -----------------------------------------------------------------------
 * Tests: llm_tool_build_tools_json — tool with params
 * ----------------------------------------------------------------------- */

static void test_build_tools_json_with_params_has_parameters_key(void)
{
    TkToolParam params[2];
    params[0].name        = "location";
    params[0].type        = "string";
    params[0].description = "City name";
    params[0].required    = 1;

    params[1].name        = "unit";
    params[1].type        = "string";
    params[1].description = "Temperature unit";
    params[1].required    = 0;

    TkToolDecl tool;
    tool.name        = "get_weather";
    tool.description = "Get the current weather";
    tool.params      = params;
    tool.nparams     = 2;

    const char *json = llm_tool_build_tools_json(&tool, 1);
    ASSERT_CONTAINS(json, "\"parameters\"",
                    "build_tools_json with params: contains parameters key");
    free((char *)json);
}

static void test_build_tools_json_with_params_has_param_name(void)
{
    TkToolParam param;
    param.name        = "location";
    param.type        = "string";
    param.description = "City name";
    param.required    = 1;

    TkToolDecl tool;
    tool.name        = "get_weather";
    tool.description = "Get the current weather";
    tool.params      = &param;
    tool.nparams     = 1;

    const char *json = llm_tool_build_tools_json(&tool, 1);
    ASSERT_CONTAINS(json, "location",
                    "build_tools_json with params: contains param name");
    free((char *)json);
}

static void test_build_tools_json_required_param_in_required_array(void)
{
    TkToolParam param;
    param.name        = "location";
    param.type        = "string";
    param.description = "City name";
    param.required    = 1;

    TkToolDecl tool;
    tool.name        = "get_weather";
    tool.description = "Get the current weather";
    tool.params      = &param;
    tool.nparams     = 1;

    const char *json = llm_tool_build_tools_json(&tool, 1);
    /* "required":["location"] */
    ASSERT_CONTAINS(json, "\"required\"",
                    "build_tools_json required param: required array present");
    ASSERT_CONTAINS(json, "\"location\"",
                    "build_tools_json required param: name appears in required context");
    free((char *)json);
}

static void test_build_tools_json_optional_param_not_in_required_array(void)
{
    TkToolParam params[2];
    params[0].name        = "location";
    params[0].type        = "string";
    params[0].description = "City name";
    params[0].required    = 1;

    params[1].name        = "unit";
    params[1].type        = "string";
    params[1].description = "Temperature unit";
    params[1].required    = 0;  /* optional */

    TkToolDecl tool;
    tool.name        = "get_weather";
    tool.description = "Get the current weather";
    tool.params      = params;
    tool.nparams     = 2;

    const char *json = llm_tool_build_tools_json(&tool, 1);
    /* The required array should contain "location" but NOT "unit".
     * We verify by checking that "unit" does NOT appear inside the required array.
     * A simple proxy: the required array ends with "location"] and does not
     * contain "unit" between "required":[ and the first ]. */
    const char *req_ptr = strstr(json, "\"required\":[");
    ASSERT(req_ptr != NULL, "build_tools_json optional param: required key exists");
    if (req_ptr) {
        const char *arr_end = strchr(req_ptr, ']');
        /* Extract just the required array content */
        if (arr_end) {
            size_t arr_len = (size_t)(arr_end - req_ptr);
            char  *arr_buf = malloc(arr_len + 1);
            if (arr_buf) {
                memcpy(arr_buf, req_ptr, arr_len);
                arr_buf[arr_len] = '\0';
                ASSERT(strstr(arr_buf, "\"unit\"") == NULL,
                       "build_tools_json optional param: unit NOT in required array");
                free(arr_buf);
            }
        }
    }
    free((char *)json);
}

/* -----------------------------------------------------------------------
 * Tests: llm_tool_build_tools_json — two tools
 * ----------------------------------------------------------------------- */

static void test_build_tools_json_two_tools_both_names_present(void)
{
    TkToolDecl tools[2];
    tools[0].name        = "get_weather";
    tools[0].description = "Weather tool";
    tools[0].params      = NULL;
    tools[0].nparams     = 0;

    tools[1].name        = "send_email";
    tools[1].description = "Email tool";
    tools[1].params      = NULL;
    tools[1].nparams     = 0;

    const char *json = llm_tool_build_tools_json(tools, 2);
    ASSERT_CONTAINS(json, "get_weather",
                    "build_tools_json two tools: first tool name present");
    ASSERT_CONTAINS(json, "send_email",
                    "build_tools_json two tools: second tool name present");
    free((char *)json);
}

/* -----------------------------------------------------------------------
 * Tests: llm_parse_tool_calls — one tool call
 * ----------------------------------------------------------------------- */

static void test_parse_tool_calls_one_call_ncalls_is_1(void)
{
    ToolCallResult r = llm_parse_tool_calls(RESPONSE_ONE_TOOL_CALL);
    ASSERT(r.ncalls == 1,
           "parse_tool_calls one call: ncalls == 1");
    ASSERT(r.is_err == 0,
           "parse_tool_calls one call: is_err == 0");
}

static void test_parse_tool_calls_one_call_name_correct(void)
{
    ToolCallResult r = llm_parse_tool_calls(RESPONSE_ONE_TOOL_CALL);
    ASSERT(r.ncalls >= 1 && r.calls != NULL,
           "parse_tool_calls one call: calls array non-null");
    if (r.ncalls >= 1 && r.calls) {
        ASSERT_STREQ(r.calls[0].tool_name, "get_weather",
                     "parse_tool_calls one call: tool_name == get_weather");
    }
}

static void test_parse_tool_calls_one_call_id_matches(void)
{
    ToolCallResult r = llm_parse_tool_calls(RESPONSE_ONE_TOOL_CALL);
    if (r.ncalls >= 1 && r.calls) {
        ASSERT_STREQ(r.calls[0].call_id, "call_xyz999",
                     "parse_tool_calls one call: call_id matches fixture");
    } else {
        ASSERT(0, "parse_tool_calls one call: no calls to check call_id");
    }
}

static void test_parse_tool_calls_one_call_args_json_nonempty(void)
{
    ToolCallResult r = llm_parse_tool_calls(RESPONSE_ONE_TOOL_CALL);
    if (r.ncalls >= 1 && r.calls) {
        ASSERT(r.calls[0].args_json != NULL && strlen(r.calls[0].args_json) > 0,
               "parse_tool_calls one call: args_json is non-empty");
    } else {
        ASSERT(0, "parse_tool_calls one call: no calls to check args_json");
    }
}

/* -----------------------------------------------------------------------
 * Tests: llm_parse_tool_calls — no tool calls
 * ----------------------------------------------------------------------- */

static void test_parse_tool_calls_no_tool_calls_ncalls_is_0(void)
{
    ToolCallResult r = llm_parse_tool_calls(RESPONSE_NO_TOOL_CALLS);
    ASSERT(r.ncalls == 0,
           "parse_tool_calls no tool calls: ncalls == 0");
    ASSERT(r.is_err == 0,
           "parse_tool_calls no tool calls: is_err == 0");
}

/* -----------------------------------------------------------------------
 * Tests: llm_parse_tool_calls — two tool calls
 * ----------------------------------------------------------------------- */

static void test_parse_tool_calls_two_calls_ncalls_is_2(void)
{
    ToolCallResult r = llm_parse_tool_calls(RESPONSE_TWO_TOOL_CALLS);
    ASSERT(r.ncalls == 2,
           "parse_tool_calls two calls: ncalls == 2");
}

/* -----------------------------------------------------------------------
 * Tests: llm_tool_result_msgs
 * ----------------------------------------------------------------------- */

static void test_tool_result_msgs_one_result_returns_one_message(void)
{
    TkToolResult result;
    result.call_id     = "call_xyz999";
    result.result_json = "{\"temperature\":22,\"unit\":\"celsius\"}";

    uint64_t  nmsgs = 0;
    TkLlmMsg *msgs  = llm_tool_result_msgs(&result, 1, &nmsgs);

    ASSERT(nmsgs == 1,
           "tool_result_msgs one result: nmsgs == 1");
    ASSERT(msgs != NULL,
           "tool_result_msgs one result: msgs non-null");
    free((char *)msgs);
}

static void test_tool_result_msgs_one_result_role_is_tool(void)
{
    TkToolResult result;
    result.call_id     = "call_xyz999";
    result.result_json = "{\"temperature\":22,\"unit\":\"celsius\"}";

    uint64_t  nmsgs = 0;
    TkLlmMsg *msgs  = llm_tool_result_msgs(&result, 1, &nmsgs);

    if (msgs && nmsgs >= 1) {
        ASSERT_STREQ(msgs[0].role, "tool",
                     "tool_result_msgs one result: role == tool");
        free((char *)msgs[0].role);
        free((char *)msgs[0].content);
    } else {
        ASSERT(0, "tool_result_msgs one result: no msgs to check role");
    }
    free(msgs);
}

static void test_tool_result_msgs_content_contains_result_json(void)
{
    TkToolResult result;
    result.call_id     = "call_xyz999";
    result.result_json = "{\"temperature\":22}";

    uint64_t  nmsgs = 0;
    TkLlmMsg *msgs  = llm_tool_result_msgs(&result, 1, &nmsgs);

    if (msgs && nmsgs >= 1) {
        ASSERT_CONTAINS(msgs[0].content, "{\"temperature\":22}",
                        "tool_result_msgs: content contains result_json value");
        free((char *)msgs[0].role);
        free((char *)msgs[0].content);
    } else {
        ASSERT(0, "tool_result_msgs: no msgs to check content");
    }
    free(msgs);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(void)
{
    /* llm_tool_build_tools_json — single tool */
    test_build_tools_json_single_tool_has_type_function();
    test_build_tools_json_single_tool_has_name();
    test_build_tools_json_single_tool_has_description();

    /* llm_tool_build_tools_json — tool with params */
    test_build_tools_json_with_params_has_parameters_key();
    test_build_tools_json_with_params_has_param_name();
    test_build_tools_json_required_param_in_required_array();
    test_build_tools_json_optional_param_not_in_required_array();

    /* llm_tool_build_tools_json — two tools */
    test_build_tools_json_two_tools_both_names_present();

    /* llm_parse_tool_calls — one tool call */
    test_parse_tool_calls_one_call_ncalls_is_1();
    test_parse_tool_calls_one_call_name_correct();
    test_parse_tool_calls_one_call_id_matches();
    test_parse_tool_calls_one_call_args_json_nonempty();

    /* llm_parse_tool_calls — no tool calls */
    test_parse_tool_calls_no_tool_calls_ncalls_is_0();

    /* llm_parse_tool_calls — two tool calls */
    test_parse_tool_calls_two_calls_ncalls_is_2();

    /* llm_tool_result_msgs */
    test_tool_result_msgs_one_result_returns_one_message();
    test_tool_result_msgs_one_result_role_is_tool();
    test_tool_result_msgs_content_contains_result_json();

    printf("\n%s — %d failure(s)\n", failures == 0 ? "OK" : "FAILED", failures);
    return failures ? 1 : 0;
}
