# std.llm.tool

LLM tool-use support: declare tools, execute tool-calling chat loops, parse tool calls, and submit results.

## Types

```
type tooldecl   { name: str, desc: str, params: [toolparam] }
type toolparam  { name: str, type: str, desc: str, required: bool }
type toolcall   { name: str, args: [[str]], id: str }
type toolresult { id: str, content: str, error: bool }
```

## Functions

```
f=llm.withtools(c:llm.llmclient;tools:[tooldecl]):llm.llmclient
f=llm.chatwithtools(c:llm.llmclient;msgs:[llm.llmmsg]):toolcall!llm.llmerr
f=llm.submitresult(c:llm.llmclient;msgs:[llm.llmmsg];result:toolresult):llm.llmresp!llm.llmerr
f=llm.parsetoolcalls(raw:str):toolcall!llm.llmerr
f=llm.resultmsgs(results:[toolresult]):[llm.llmmsg]
```

## Semantics

- `withtools` returns a new client configured with the given tool declarations. The original client is unchanged.
- `chatwithtools` sends a chat request with tool declarations and returns the first tool call the model wants to invoke. Returns `llmerr` if the model responds without a tool call or on network failure.
- `submitresult` sends a tool result back to the model and returns the model's follow-up response.
- `parsetoolcalls` extracts a `toolcall` from raw model output text (for models that return tool calls as text rather than structured output).
- `resultmsgs` converts tool results into `llmmsg` entries suitable for appending to a conversation history.
- `toolcall.args` is a list of key-value pairs (name, value as string).

## Dependencies

- `std.llm` (client, message, response, and error types)
