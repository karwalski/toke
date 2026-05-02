---
title: std.llm_tool
slug: llm_tool
section: reference/stdlib
order: 23
---

**Status: Implemented** -- C runtime backing.

The `std.llm_tool` module extends `std.llm` with OpenAI-compatible tool/function-calling support. It provides the full round-trip flow: declare tool schemas, send a chat request that includes those tools, parse the model's tool-call decision from the response, execute the requested tools in your own code, and return the results so the model can produce a final answer.

## Tool-Call Flow

Tool calling follows five steps every time the model needs to invoke external logic:

1. **Define tools** -- build an array of `$tooldecl` values describing each callable function.
2. **Send to LLM** -- call `llm.withtools` to attach the tools to a client, then `llm.chatwithtools` to send the conversation.
3. **Parse tool calls** -- the model responds with a `$toolcall` value naming the function and its arguments.
4. **Execute tools** -- run the requested function in toke code and capture the result.
5. **Send results back** -- call `llm.submitresult` with the `$toolresult` to get the model's final text response.

## Types

### $tooldecl

Declares one callable tool that the model may choose to invoke.

| Field | Type | Meaning |
|-------|------|---------|
| name | $str | Tool name (used by the model to request the call) |
| desc | $str | Human-readable description that guides the model's decision |
| params | @($toolparam) | Parameter definitions |

### $toolparam

Describes one parameter of a tool declaration, following JSON Schema conventions for the `type` field.

| Field | Type | Meaning |
|-------|------|---------|
| name | $str | Parameter name |
| type | $str | JSON Schema type: `"string"`, `"number"`, `"boolean"`, `"array"`, `"object"` |
| desc | $str | Description sent to the model to explain the parameter's purpose |
| required | bool | `true` if the model must always supply this argument |

### $toolcall

Represents a single tool invocation requested by the model. The `id` field must be echoed back in the corresponding `$toolresult`.

| Field | Type | Meaning |
|-------|------|---------|
| name | $str | Name of the tool the model wants to call |
| args | @(@($str)) | Argument key-value pairs extracted from the model's JSON |
| id | $str | Call identifier for correlating results back to requests |

### $toolresult

Carries the output of one executed tool call back to the model.

| Field | Type | Meaning |
|-------|------|---------|
| id | $str | Call identifier from the matching `$toolcall.id` |
| content | $str | Result content (typically a JSON string) |
| error | bool | `true` if the tool execution failed; the model will see an error indication |

## Functions

### llm.withtools(c: $llmclient; tools: @($tooldecl)): $llmclient

Returns a new `$llmclient` that carries the given tool declarations; the original client is unchanged. All subsequent `llm.chatwithtools` calls on the returned client will include these tool schemas in the request. Calling `llm.withtools` again on the result replaces the tool list.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let p=$toolparam{name:"city";type:"string";desc:"City name";required:true};
  let tool=$tooldecl{name:"getweather";desc:"Return current weather";params:@(p)};
  let c2=llm.withtools(c;@(tool));
};
```

### llm.chatwithtools(c: $llmclient; msgs: @($llmmsg)): $toolcall!$llmerr

Sends a chat request to the model including the tool schemas attached to `c` and returns the `$toolcall` the model has chosen to make. Returns `$llmerr` if the request fails or if the model did not request a tool call (use `llm.chat` for ordinary responses). When the model requests multiple tool calls, only the first is returned; use `llm.parsetoolcalls` to handle the raw response if you need all calls.

<!-- skip-check -->
```toke
m=main;
i=llm:std.llm;
i=env:std.env;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let p=$toolparam{name:"city";type:"string";desc:"City name";required:true};
  let tool=$tooldecl{name:"getweather";desc:"Return weather for a city";params:@(p)};
  let c2=llm.withtools(c;@(tool));
  let msgusr=$llmmsg{role:"user";content:"What is the weather in Sydney?"};
  let msgs=@(msgusr);
  let emptycall=$toolcall{name:"";args:@();id:""};
  let call=llm.chatwithtools(c2;msgs)|{$ok:tc tc;$err:e emptycall};
};
```

### llm.submitresult(c: $llmclient; msgs: @($llmmsg); result: $toolresult): $llmresp!$llmerr

Sends the original conversation messages plus one `$toolresult` back to the model and returns the model's final text response. The `result.id` must match the `id` from the `$toolcall` returned by `llm.chatwithtools`. Returns `$llmerr` on network or provider failure.

<!-- skip-check -->
```toke
m=main;
i=llm:std.llm;
i=env:std.env;
i=log:std.log;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let p=$toolparam{name:"city";type:"string";desc:"City name";required:true};
  let tool=$tooldecl{name:"getweather";desc:"Return weather for a city";params:@(p)};
  let c2=llm.withtools(c;@(tool));
  let msgusr=$llmmsg{role:"user";content:"What is the weather in Sydney?"};
  let msgs=@(msgusr);
  let emptycall=$toolcall{name:"";args:@();id:""};
  let call=llm.chatwithtools(c2;msgs)|{$ok:tc tc;$err:e emptycall};
  let result=$toolresult{id:call.id;content:"{\"temp_c\": 22, \"condition\": \"sunny\"}";error:false};
  let emptyresp=$llmresp{content:"";tokensin:0;tokensout:0;model:""};
  let resp=llm.submitresult(c2;msgs;result)|{$ok:r r;$err:e emptyresp};
  log.info(resp.content;@());
};
```

### llm.parsetoolcalls(raw: $str): $toolcall!$llmerr

Parses the `tool_calls` array from a raw JSON response string and returns the first `$toolcall`. Use this when you have the raw model response text (e.g. from `llm.chat`) rather than going through `llm.chatwithtools`. Returns `$llmerr` if the string is not valid JSON or contains no tool calls.

<!-- skip-check -->
```toke
m=main;
i=llm:std.llm;
i=env:std.env;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let msgusr=$llmmsg{role:"user";content:"Hello"};
  let msgs=@(msgusr);
  let emptyresp=$llmresp{content:"";tokensin:0;tokensout:0;model:""};
  let resp=llm.chat(c;msgs)|{$ok:r r;$err:e emptyresp};
  let emptycall=$toolcall{name:"";args:@();id:""};
  let call=llm.parsetoolcalls(resp.content)|{$ok:tc tc;$err:e emptycall};
};
```

### llm.resultmsgs(results: @($toolresult)): @($llmmsg)

Converts an array of `$toolresult` values into `$llmmsg` values with `role="tool"`, ready to be appended to the conversation history before the next request. Use this when managing multi-turn conversations that involve several consecutive tool calls.

<!-- skip-check -->
```toke
m=main;
i=llm:std.llm;

f=demo():void{
  let result=$toolresult{id:"call-1";content:"{\"ok\":true}";error:false};
  let toolmsgs=llm.resultmsgs(@(result));
};
```

## Complete End-to-End Example: Weather Query Tool

This example demonstrates the full five-step tool-call flow using a weather lookup tool. The tool is defined, the model is asked a weather question, the model requests the tool, toke executes it with a hardcoded response, and the result is submitted back to get a natural-language answer.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;
i=log:std.log;

f=main():i64{
  let pcity=$toolparam{name:"city";type:"string";desc:"City name";required:true};
  let punits=$toolparam{name:"units";type:"string";desc:"celsius or fahrenheit";required:false};
  let wtool=$tooldecl{name:"getweather";desc:"Return weather for a city";params:@(pcity;punits)};
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let base=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let c=llm.withtools(base;@(wtool));
  let msgsys=$llmmsg{role:"system";content:"You are a weather assistant. Use tools to answer."};
  let msgusr=$llmmsg{role:"user";content:"What is the weather in Sydney right now?"};
  let msgs=@(msgsys;msgusr);
  let emptycall=$toolcall{name:"";args:@();id:""};
  let call=llm.chatwithtools(c;msgs)|{$ok:tc tc;$err:e emptycall};
  let weatherjson="{\"city\":\"Sydney\",\"temp_c\":22,\"condition\":\"partly cloudy\"}";
  let result=$toolresult{id:call.id;content:weatherjson;error:false};
  let emptyresp=$llmresp{content:"";tokensin:0;tokensout:0;model:""};
  let resp=llm.submitresult(c;msgs;result)|{$ok:r r;$err:e emptyresp};
  log.info(resp.content;@());
  <0;
};
```

## See Also

- `std.llm` -- base LLM client, message types, and chat functions used throughout this module.
