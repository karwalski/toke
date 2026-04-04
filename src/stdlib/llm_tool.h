#ifndef TK_STDLIB_LLM_TOOL_H
#define TK_STDLIB_LLM_TOOL_H

/*
 * llm_tool.h — C interface for the std.llm.tool standard library module.
 *
 * Provides OpenAI-compatible tool/function-calling support on top of the
 * base std.llm chat client.
 *
 * Type mappings:
 *   Str  = const char *  (null-terminated UTF-8)
 *   bool = int  (0 = false, 1 = true)
 *   u64  = uint64_t
 *
 * malloc is permitted; callers own returned heap pointers unless noted.
 *
 * No external dependencies beyond libc and llm.h.
 *
 * Story: 17.1.2
 */

#include "llm.h"
#include <stdint.h>

/* TkToolParam — a single parameter for a tool/function declaration. */
typedef struct {
    const char *name;
    const char *type;        /* "string", "number", "boolean", "array", "object" */
    const char *description;
    int         required;    /* 1 = required */
} TkToolParam;

/* TkToolDecl — declaration of a callable tool/function. */
typedef struct {
    const char   *name;
    const char   *description;
    TkToolParam  *params;
    uint64_t      nparams;
} TkToolDecl;

/* TkToolCall — a single tool invocation requested by the model. */
typedef struct {
    const char *tool_name;
    const char *call_id;    /* from response */
    const char *args_json;  /* raw JSON object string */
} TkToolCall;

/* TkToolResult — the result of executing a tool call. */
typedef struct {
    const char *call_id;
    const char *result_json; /* JSON string to return as tool result */
} TkToolResult;

/* ToolCallResult — return type from llm_chatwithtools and llm_parse_tool_calls. */
typedef struct {
    TkToolCall *calls;
    uint64_t    ncalls;
    int         is_err;
    const char *err_msg;
} ToolCallResult;

/* llm_tool_build_tools_json — build OpenAI tools JSON array from declarations.
 * Returns a heap-allocated string the caller must free, or NULL on error. */
const char    *llm_tool_build_tools_json(TkToolDecl *tools, uint64_t ntools);

/* llm_chatwithtools — make a chat request with tools; return parsed tool calls
 * if the model chose to call a tool. */
ToolCallResult llm_chatwithtools(TkLlmClient *c, TkLlmMsg *msgs, uint64_t nmsgs,
                                  TkToolDecl *tools, uint64_t ntools);

/* llm_submitresult — submit tool results back to the model and return the
 * next completion. */
TkLlmResp      llm_submitresult(TkLlmClient *c, TkLlmMsg *msgs, uint64_t nmsgs,
                                 TkToolDecl *tools, uint64_t ntools,
                                 TkToolResult *results, uint64_t nresults);

/* llm_parse_tool_calls — parse tool_calls array from a raw response JSON string. */
ToolCallResult llm_parse_tool_calls(const char *response_json);

/* llm_tool_result_msgs — build TkLlmMsg array (role="tool") for each result,
 * to be appended to the conversation before the next request.
 * Sets *nmsgs_out to the number of messages returned.
 * Returns a heap-allocated array the caller must free (each msg->content is also
 * heap-allocated), or NULL on error. */
TkLlmMsg      *llm_tool_result_msgs(TkToolResult *results, uint64_t nresults,
                                     uint64_t *nmsgs_out);

#endif /* TK_STDLIB_LLM_TOOL_H */
