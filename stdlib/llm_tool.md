# std.llm.tool -- Tool/Function Calling

## Overview

The `std.llm.tool` module extends `std.llm` with the tool-use (function-calling) protocol supported by OpenAI, Anthropic, and compatible APIs. Tools are declared using `tooldecl` structs, attached to a client via `llm.withtools()`, and invoked when the model decides a tool call is needed. The caller receives a `toolcall`, executes the corresponding function, and returns a `toolresult` to continue the conversation. Depends on `std.llm`.

## Types

### tooldecl

Declares a tool that the LLM may invoke.

| Field | Type | Meaning |
|-------|------|---------|
| name | Str | Tool name (snake_case, matches function call in LLM output) |
| desc | Str | Human-readable description shown to the model |
| params | @toolparam | Ordered list of parameter declarations |

### toolparam

A single parameter in a tool declaration.

| Field | Type | Meaning |
|-------|------|---------|
| name | Str | Parameter name |
| type | Str | JSON Schema type (`"str"`, `"u64"`, `"bool"`, etc.) |
| desc | Str | Description shown to the model |
| required | bool | Whether the parameter must be present |

### toolcall

A tool invocation requested by the model.

| Field | Type | Meaning |
|-------|------|---------|
| name | Str | Name of the tool to call |
| args | @(@Str) | Key-value argument pairs |
| id | Str | Unique call ID (must be echoed in `toolresult.id`) |

### toolresult

The result of executing a tool, to be submitted back to the model.

| Field | Type | Meaning |
|-------|------|---------|
| id | Str | Call ID from the corresponding `toolcall` |
| content | Str | Result payload (typically JSON) |
| error | bool | `true` if the tool execution failed |

## Functions

### llm.withtools(c: llmclient; tools: @tooldecl): llmclient

Returns a copy of the client with the given tools registered. The original client is unchanged.

**Example:**
```toke
let weather_tool = $tooldecl{
  name: "get_weather";
  desc: "Get current weather for a city";
  params: @($toolparam{name: "city"; type: "str"; desc: "City name"; required: true})
};
let c2 = llm.withtools(c; @(weather_tool));
```

### llm.chatwithtools(c: llmclient; msgs: @llmmsg): toolcall!llmerr

Sends a chat request with tools available. If the model decides to call a tool, returns a `toolcall`. If the model responds with text instead, the content is returned as a `toolcall` with `name == ""`.

**Example:**
```toke
let call = llm.chatwithtools(c2; msgs)?;
if call.name == "get_weather" {
  let city = call.args[0][1];
  let result = $toolresult{id: call.id; content: fetch_weather(city); error: false};
  let final = llm.submitresult(c2; msgs; result)?;
  log.info(final.content);
};
```

### llm.submitresult(c: llmclient; msgs: @llmmsg; result: toolresult): llmresp!llmerr

Submits a `toolresult` back to the model and returns the final text response.
