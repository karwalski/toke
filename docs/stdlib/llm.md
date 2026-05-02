---
title: std.llm
slug: llm
section: reference/stdlib
order: 22
---

**Status: Implemented** -- C runtime backing.

The `std.llm` module provides a provider-agnostic client for large language model APIs over an OpenAI-compatible HTTP interface. It supports multi-turn chat, streaming token delivery, and single-prompt completion. Local models served via Ollama (`http://localhost:11434/v1`) use the same client as hosted providers.

> **Note:** `llm.tki` exports six functions: `llm.client`, `llm.chat`, `llm.chatstream`, `llm.streamnext`, `llm.complete`, and `llm.countokens`. Extended features (`llm.embedding`, `llm.json_mode`, `llm.vision`, `llm.retry_backoff`, `llm.usage`) are planned but not yet in the current tki -- do not use them.

## Types

### $llmclient

Holds all connection parameters for one LLM provider endpoint. Create with `llm.client`; the handle is reused across multiple requests.

| Field | Type | Meaning |
|-------|------|---------|
| provider | $str | Provider name (e.g. `"openai"`, `"anthropic"`, `"ollama"`) |
| model | $str | Model identifier (e.g. `"gpt-4o"`, `"claude-3-5-sonnet-20241022"`) |

The runtime also stores the API key, base URL, and timeout internally; these are not directly accessible as toke fields.

### $llmmsg

A single message in a conversation turn.

| Field | Type | Meaning |
|-------|------|---------|
| role | $str | `"system"`, `"user"`, or `"assistant"` |
| content | $str | Message text |

### $llmresp

The result of a non-streaming chat completion.

| Field | Type | Meaning |
|-------|------|---------|
| content | $str | Full response text |
| tokensin | u64 | Input tokens consumed for this request |
| tokensout | u64 | Output tokens generated for this request |
| model | $str | Model that produced the response |

### $llmstream

Opaque handle to a streaming response. Consume chunks one at a time with `llm.streamnext` until an empty string is returned.

### $llmerr

Returned when a request fails, including network errors, authentication failures, and provider error responses.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable error description |
| code | u64 | Provider HTTP or application error code |

## Functions

### llm.client(baseurl: $str; apikey: $str; model: $str): $llmclient

Creates a new LLM client configured for the given provider endpoint, authentication key, and model. For local Ollama models pass an empty string for `apikey`. The default request timeout is 30 000 ms.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;
f=demo():i64{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  < 0
};
```

### llm.chat(c: $llmclient; msgs: @($llmmsg)): $llmresp!$llmerr

Sends a multi-turn conversation to the model and returns a single `$llmresp` containing the full response text and token counts. Returns `$llmerr` on network failure, authentication error, or provider rejection.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;
i=log:std.log;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let msgsys=$llmmsg{role:"system";content:"You are a toke language assistant."};
  let msgusr=$llmmsg{role:"user";content:"What is a result type?"};
  let msgs=@(msgsys;msgusr);
  let resp=llm.chat(c;msgs)|{$ok:r r;$err:e $llmresp{content:"";tokensin:0;tokensout:0;model:""}};
  log.info(resp.content;@());
};
```

### llm.chatstream(c: $llmclient; msgs: @($llmmsg)): $llmstream!$llmerr

Sends a chat request with `stream: true` and returns an `$llmstream` handle holding the collected SSE delta chunks. Use `llm.streamnext` to consume chunks in order. Returns `$llmerr` if the connection fails before any data arrives.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let msgusr=$llmmsg{role:"user";content:"Hello"};
  let msgs=@(msgusr;msgusr);
  let stream=llm.chatstream(c;msgs)|{$ok:s s;$err:e $llmstream{}};
};
```

### llm.streamnext(s: $llmstream): $str!$llmerr

Returns the next delta chunk from the stream and advances the internal cursor. Returns an empty string `""` when all chunks have been consumed, signalling that the response is complete. Returns `$llmerr` if the stream was opened in an error state.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;
i=io:std.io;
i=str:std.str;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let msgusr=$llmmsg{role:"user";content:"Hello"};
  let msgs=@(msgusr;msgusr);
  let stream=llm.chatstream(c;msgs)|{$ok:s s;$err:e $llmstream{}};
  lp(let n=0;n<4096;n=n+1){
    let chunk=llm.streamnext(stream)|{$ok:t t;$err:e ""};
    if(str.len(chunk)=0){br;};
    io.write(chunk);
  };
};
```

### llm.complete(c: $llmclient; prompt: $str): $str!$llmerr

Convenience wrapper that sends `prompt` as a single user message and returns the response text directly, skipping the `$llmresp` wrapper. Useful for one-shot prompt/response patterns where token counts are not needed.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let answer=llm.complete(c;"Summarise the toke type system.")|{$ok:s s;$err:e ""};
};
```

### llm.countokens(c: $llmclient; text: $str): u64

Returns an approximate token count for `text` using the heuristic `strlen(text) / 4`. This is fast and requires no network call; use it for pre-flight size checks before submitting large prompts.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;

f=demo():void{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let n=llm.countokens(c;"some large text here");
};
```

## Usage Examples

### 1. Simple Chat

The basic pattern: create a client, build a message array, send with `llm.chat`, and handle the result type.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;
i=log:std.log;

f=main():i64{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let msgsys=$llmmsg{role:"system";content:"You are a helpful assistant."};
  let msgusr=$llmmsg{role:"user";content:"What is the toke language?"};
  let msgs=@(msgsys;msgusr);
  let resp=llm.chat(c;msgs)|{$ok:r r;$err:e $llmresp{content:"";tokensin:0;tokensout:0;model:""}};
  log.info(resp.content;@());
  <0;
};
```

### 2. Streaming Response

Use `llm.chatstream` and `llm.streamnext` in a loop to print tokens as they arrive, reducing time-to-first-output.

```toke
m=main;
i=llm:std.llm;
i=env:std.env;
i=io:std.io;
i=str:std.str;

f=main():i64{
  let key=env.get("OPENAI_KEY")|{$ok:k k;$err:e ""};
  let c=llm.client("https://api.openai.com/v1";key;"gpt-4o");
  let msgusr=$llmmsg{role:"user";content:"Write a haiku about compilers."};
  let msgs=@(msgusr;msgusr);
  let stream=llm.chatstream(c;msgs)|{$ok:s s;$err:e llm.emptystream()};
  lp(let n=0;n<4096;n=n+1){
    let chunk=llm.streamnext(stream)|{$ok:t t;$err:e ""};
    if(str.len(chunk)=0){br;};
    io.write(chunk);
  };
  io.writeln("");
  <0;
};
```

## See Also

- `std.llm_tool` -- tool/function-calling support built on top of this module.
