#ifndef TK_STDLIB_LLMTOOL_H
#define TK_STDLIB_LLMTOOL_H

/*
 * llmtool.h — C interface for the std.llmtool standard library module.
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

/* -----------------------------------------------------------------------
 * Story 32.2.1 — parallel tool calls, arg validation, agentic loop
 * ----------------------------------------------------------------------- */

/* TkTool — a callable tool with a handler function pointer. */
typedef struct {
    const char  *name;
    const char  *description;
    const char  *parameters_json; /* JSON schema object for parameters, may be NULL */
    /* handler: called with args_json; returns heap-allocated result JSON string */
    const char *(*handler)(const char *args_json);
} TkTool;

/* TkToolCallResult — result of a single tool invocation. */
typedef struct {
    const char *call_id;
    const char *tool_name;
    const char *result_json; /* heap-allocated, caller must free */
    int         is_err;
    const char *err_msg;
} TkToolCallResult;

/* ToolCallResultArray — array of TkToolCallResult returned by llm_parallel_tool_calls. */
typedef struct {
    TkToolCallResult *data;
    uint64_t          len;
} ToolCallResultArray;

/* ToolValidResult — return type from llm_tool_validate_args. */
typedef struct {
    int         ok;
    int         is_err;
    const char *err_msg;
} ToolValidResult;

/* llm_parallel_tool_calls — call llm_chat with the given messages, then for each
 * tool_call in the response, dispatch to the matching TkTool handler.
 * Returns a heap-allocated ToolCallResultArray; caller owns data and must free
 * each result_json as well as data itself. */
ToolCallResultArray llm_parallel_tool_calls(TkLlmClient *client,
                                             TkLlmMsg *messages, uint64_t nmsg,
                                             TkTool *tools, uint64_t ntools);

/* llm_tool_validate_args — validate args_json against tool->parameters_json schema.
 * Checks that all fields listed in parameters.required are present in args_json.
 * Returns ok=1 on success, is_err=1 with err_msg on failure. */
ToolValidResult llm_tool_validate_args(TkTool *tool, const char *args_json);

/* llm_agentic_loop — ReAct-style agentic loop.
 * Sends system+user messages to the model; if the model requests tool calls,
 * executes them and continues; repeats up to max_iterations times.
 * Returns heap-allocated final text content, or "[max iterations reached]",
 * or an error string prefixed with "[error]". Caller must free. */
const char *llm_agentic_loop(TkLlmClient *client,
                              const char *system,
                              const char *user,
                              TkTool *tools, uint64_t ntools,
                              uint64_t max_iterations);

/* -----------------------------------------------------------------------
 * .tki-aligned aliases (Story 35.1.12)
 *
 * The std.llmtool .tki contract exports names like llm.withtools,
 * llm.chatwithtools, etc.  The compiler's resolve_stdlib_call maps these
 * to the llm_tool_<method> C symbol convention.  The #defines below
 * provide those canonical names while keeping the original symbols for
 * backward compatibility.
 * ----------------------------------------------------------------------- */

/* llm.withtools → llm_tool_withtools */
#define llm_tool_withtools       llm_tool_build_tools_json

/* llm.chatwithtools → llm_tool_chatwithtools */
#define llm_tool_chatwithtools   llm_chatwithtools

/* llm.submitresult → llm_tool_submitresult */
#define llm_tool_submitresult    llm_submitresult

/* llm.parsetoolcalls → llm_tool_parsetoolcalls */
#define llm_tool_parsetoolcalls  llm_parse_tool_calls

/* llm.resultmsgs → llm_tool_resultmsgs */
#define llm_tool_resultmsgs      llm_tool_result_msgs

#endif /* TK_STDLIB_LLMTOOL_H */
