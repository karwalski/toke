# std.llm

LLM client for chat completions, streaming, text completion, and token counting. Provider-agnostic interface.

## Types

```
type llmclient { provider: str, model: str }
type llmmsg    { role: str, content: str }
type llmresp   { content: str, tokens_in: u64, tokens_out: u64, model: str }
type llmerr    { msg: str, code: u64 }
type llmstream { id: u64 }  -- opaque
```

## Functions

```
f=client(provider:str;model:str;apikey:str):llmclient
f=chat(c:llmclient;msgs:[llmmsg]):llmresp!llmerr
f=chatstream(c:llmclient;msgs:[llmmsg]):llmstream!llmerr
f=streamnext(s:llmstream):str!llmerr
f=complete(c:llmclient;prompt:str):str!llmerr
f=countokens(c:llmclient;text:str):u64
```

## Semantics

- `client` creates an LLM client for the given provider (`"openai"`, `"anthropic"`, etc.), model name, and API key.
- `chat` sends a conversation and blocks until the full response is available. Returns `llmerr` on network failure, auth error, or rate limit (error `code` mirrors the HTTP status).
- `chatstream` opens a streaming connection. Use `streamnext` to pull chunks; returns empty string `""` when the stream is complete, `llmerr` on failure.
- `complete` is a convenience for single-prompt completion (wraps a single user message).
- `countokens` estimates the token count for the given text using the model's tokenizer.

## Dependencies

- `std.http` (HTTP client for API calls)
- `std.json` (request/response serialization)
