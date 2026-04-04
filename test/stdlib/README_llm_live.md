# std.llm Live Integration Tests (Story 17.1.3)

These tests make real HTTP requests to LLM provider APIs. They are **not** part
of the default test suite and must be run explicitly.

## How to run

```sh
make test-stdlib-llm-live
```

## Required environment variables

Each test checks its own env var and **skips automatically** if it is not set.
You only need to export the keys for the providers you want to test.

| Variable | Provider | Where to get a key |
|---|---|---|
| `OPENAI_API_KEY` | OpenAI (`api.openai.com`) | <https://platform.openai.com/api-keys> |
| `ANTHROPIC_API_KEY` | Anthropic (`api.anthropic.com`) | <https://console.anthropic.com/settings/keys> |
| `OLLAMA_HOST` | Local Ollama server | Run `ollama serve` locally; default is `http://localhost:11434` |

## Examples

Run only the OpenAI tests (others are skipped):

```sh
OPENAI_API_KEY=sk-... make test-stdlib-llm-live
```

Run all three providers at once:

```sh
OPENAI_API_KEY=sk-... \
ANTHROPIC_API_KEY=sk-ant-... \
OLLAMA_HOST=http://localhost:11434 \
make test-stdlib-llm-live
```

Run only the Ollama tests (no cloud keys needed):

```sh
OLLAMA_HOST=http://localhost:11434 make test-stdlib-llm-live
```

## Tests included

| Test | Env var required | What it verifies |
|---|---|---|
| `test_openai_chat` | `OPENAI_API_KEY` | Non-streaming chat with `gpt-4o-mini` returns content |
| `test_openai_stream` | `OPENAI_API_KEY` | Streaming chat delivers at least 1 SSE chunk |
| `test_anthropic_chat` | `ANTHROPIC_API_KEY` | Non-streaming chat with `claude-haiku-4-5-20251001`; TLS-less builds skip gracefully |
| `test_ollama_chat` | `OLLAMA_HOST` | Non-streaming chat with `llama3.2` via local Ollama |
| `test_token_count_estimate` | none | `llm_countokens` returns > 0 for a non-empty string (no network) |

## Notes

- These tests are **opt-in** and will never run in CI unless the relevant env
  vars are explicitly injected into the CI environment.
- The Anthropic test handles builds compiled without TLS support: if
  `llm_chat` returns an error mentioning TLS/SSL/HTTPS, the test is skipped
  rather than failed.
- The `test_token_count_estimate` test requires no API key and no network
  access; it always runs.
