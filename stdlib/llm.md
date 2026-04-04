# std.llm -- LLM Client Wrapper

## Overview

The `std.llm` module provides a multi-provider abstraction for interacting with large language model APIs (OpenAI, Anthropic, local model servers, and others). It supports single-shot chat completions, streaming token-by-token responses, simple prompt completions, and token counting estimation. Retry logic with exponential backoff is handled internally. Depends on `std.http` and `std.json`.

## Types

### llmclient

A client handle bound to a specific provider and model.

| Field | Type | Meaning |
|-------|------|---------|
| provider | Str | Provider name (e.g., `"anthropic"`, `"openai"`) |
| model | Str | Model identifier (e.g., `"claude-3-5-sonnet-20241022"`) |

### llmmsg

A single chat message.

| Field | Type | Meaning |
|-------|------|---------|
| role | Str | One of `"user"`, `"assistant"`, or `"system"` |
| content | Str | Message body |

### llmresp

A completed chat response.

| Field | Type | Meaning |
|-------|------|---------|
| content | Str | Generated text |
| tokens_in | u64 | Input token count reported by the provider |
| tokens_out | u64 | Output token count reported by the provider |
| model | Str | Model that produced the response |

### llmerr

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable error description |
| code | u64 | HTTP status code or provider error code (0 if not applicable) |

### llmstream

An opaque handle to an active streaming response. Advance with `llm.streamnext()`.

## Functions

### llm.client(provider: Str; model: Str; apikey: Str): llmclient

Creates a client handle. The API key is stored securely and not exposed in the returned struct.

**Example:**
```toke
let c = llm.client("anthropic"; "claude-3-5-sonnet-20241022"; env.get("ANTHROPIC_API_KEY"));
```

### llm.chat(c: llmclient; msgs: @llmmsg): llmresp!llmerr

Sends a chat conversation and waits for a complete response.

**Example:**
```toke
let msgs = @(
  $llmmsg{role: "user"; content: "Explain monads in one sentence."}
);
let resp = llm.chat(c; msgs)?;
log.info(resp.content);
```

### llm.chatstream(c: llmclient; msgs: @llmmsg): llmstream!llmerr

Initiates a streaming chat request. Returns a stream handle immediately.

### llm.streamnext(s: llmstream): Str!llmerr

Returns the next chunk of streamed text. Returns `llmerr` when the stream is exhausted (check `code == 0` for normal end-of-stream).

**Example:**
```toke
let stream = llm.chatstream(c; msgs)?;
loop {
  let chunk = llm.streamnext(stream);
  match chunk {
    ok(t)  { str.print(t) };
    err(_) { break };
  };
};
```

### llm.complete(c: llmclient; prompt: Str): Str!llmerr

Simple single-turn text completion. Equivalent to sending a single user message and returning `resp.content`.

**Example:**
```toke
let reply = llm.complete(c; "The capital of France is")?;
```

### llm.countokens(c: llmclient; text: Str): u64

Estimates the token count for `text` using the client's tokeniser. This is a local operation and does not make a network request.

**Example:**
```toke
let n = llm.countokens(c; "Hello, world!");
```

## Error Types

### llmerr

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Error description |
| code | u64 | HTTP or provider-specific error code; 0 for end-of-stream |
