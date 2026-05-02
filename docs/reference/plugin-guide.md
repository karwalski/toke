---
title: Community Plugin Development Guide
slug: plugin-guide
section: reference
order: 10
---

This guide explains how to build plugins and integrations for the toke programming language using the hosted MCP (Model Context Protocol) service. The MCP service exposes seven tools over an SSE transport that any editor, IDE extension, or CLI tool can consume.

## MCP Tool Schemas

The toke MCP server exposes the following tools via the standard MCP `tools/list` response. Each tool accepts JSON parameters and returns a `content` array with `text` entries containing JSON.

### toke_check

Check toke source code for errors. Returns structured JSON diagnostics.

**Input:**

| Parameter | Type   | Required | Description                  |
|-----------|--------|----------|------------------------------|
| `source`  | string | yes      | Toke source code to check    |

**Output:** JSON diagnostics object. On success: `{ "raw": "...", "ok": true }`. On error: an array of diagnostics, each with `schema_version`, `error_code`, `severity`, `message`, `pos` (offset, line, col), and optional `fix` and `text` fields.

### toke_compile

Compile toke source code to LLVM IR. Returns IR on success or diagnostics on error.

**Input:**

| Parameter | Type   | Required | Description                    |
|-----------|--------|----------|--------------------------------|
| `source`  | string | yes      | Toke source code to compile    |

**Output:** On success: `{ "ok": true, "llvm_ir": "..." }`. On failure: `{ "ok": false, "diagnostics": [...] }` or `{ "ok": false, "error": "..." }`.

### toke_explain_error

Look up a toke error code and return a human-readable explanation with fix suggestions.

**Input:**

| Parameter    | Type   | Required | Description                              |
|--------------|--------|----------|------------------------------------------|
| `error_code` | string | yes      | Toke error code, e.g. `E1001` or `W1010` |

**Output:** `{ "code": "E1001", "found": true, "stage": "lex", "severity": "error", "title": "...", "explanation": "...", "fix": "..." }`. If the code is unknown: `{ "code": "...", "found": false, "message": "..." }`.

### toke_spec_lookup

Search the toke language specification by keyword. Returns the most relevant spec sections.

**Input:**

| Parameter | Type   | Required | Description                                      |
|-----------|--------|----------|--------------------------------------------------|
| `query`   | string | yes      | Search query, e.g. `arrays`, `if statement`, `type sigils` |

**Output:** `{ "query": "...", "results": [{ "title": "...", "content": "..." }, ...] }`. Returns up to 3 matching sections, ranked by relevance.

### toke_stdlib_ref

Look up a toke standard library module overview or a specific function signature and description.

**Input:**

| Parameter  | Type   | Required | Description                                    |
|------------|--------|----------|------------------------------------------------|
| `module`   | string | yes      | Module name, e.g. `str`, `db`, `crypto`, `json` |
| `function` | string | no       | Optional function name, e.g. `len`, `sha256`   |

**Output:** Module overview with all function signatures when no function is specified. Single function detail when function name is provided. Available modules: str, math, io, fs, env, time, crypto, process, log, test, db, json, toon, yaml, i18n, http.

### toke_generate

Generate toke source code from a natural-language description using AI. **Pro tier only.**

**Input:**

| Parameter     | Type   | Required | Description                                  |
|---------------|--------|----------|----------------------------------------------|
| `description` | string | yes      | What the generated code should do             |
| `difficulty`  | string | no       | Difficulty hint: `easy`, `medium`, or `hard`  |
| `max_tokens`  | number | no       | Maximum tokens for generation (default 1024)  |

**Output:** `{ "source": "...", "diagnostics": [...], "compiled": true/false, "tokens_used": N }`. The generated code is automatically validated with `toke_check`. May return `{ "cold_start": true, "estimated_wait": 45 }` if the model is warming up.

### toke_bench

Benchmark toke source code against a known task. Compares token count against a Python baseline. **Pro tier only.**

**Input:**

| Parameter | Type   | Required | Description                                                             |
|-----------|--------|----------|-------------------------------------------------------------------------|
| `source`  | string | yes      | Toke source code to benchmark                                           |
| `task_id` | string | yes      | Benchmark task ID: `hello-world`, `fizzbuzz`, `fibonacci`, `reverse-string` |

**Output:** `{ "passed": true/false, "token_count": N, "baseline_tokens": N, "token_ratio": 0.85, "diagnostics": [...], "test_results": [...] }`.

---

## Authentication

The MCP service uses API key authentication via the `Authorization` header.

### API Key Format

Keys follow the pattern `toke_{tier}_{random}`, for example:

```
toke_free_a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6
toke_pro_f6a7b8c9d0e1f2a3b4c5d6a1b2c3d4e5
```

### Sending Credentials

Include the API key as a Bearer token on both the SSE connection and any subsequent message POST requests:

```
Authorization: Bearer toke_pro_your_key_here
```

### Anonymous Access

If no API key is provided, the request is treated as anonymous free-tier access with IP-based rate limiting.

### Key States

| State    | Meaning                                             |
|----------|-----------------------------------------------------|
| Valid    | Key exists, is enabled, and has not expired          |
| Disabled | Key has been revoked — returns HTTP 401              |
| Expired  | Key TTL has elapsed — returns HTTP 401               |
| Missing  | No key provided — falls back to anonymous free tier  |

---

## Rate Limits

Limits are enforced per hour per API key (or per IP for anonymous users). When a limit is exceeded, the server returns HTTP 429 with `Retry-After` and `X-RateLimit-Reset` headers.

### Free Tier

| Tool                 | Requests / hour |
|----------------------|-----------------|
| `toke_check`         | 60              |
| `toke_compile`       | 30              |
| `toke_explain_error` | 60              |
| `toke_spec_lookup`   | 60              |
| `toke_stdlib_ref`    | 60              |
| Max SSE connections  | 2               |

Free tier does **not** have access to `toke_generate` or `toke_bench`.

### Pro Tier

| Tool                 | Requests / hour |
|----------------------|-----------------|
| `toke_check`         | 600             |
| `toke_compile`       | 300             |
| `toke_explain_error` | 600             |
| `toke_spec_lookup`   | 600             |
| `toke_stdlib_ref`    | 600             |
| `toke_generate`      | 30              |
| `toke_bench`         | 60              |
| Max SSE connections  | 10              |

### Rate Limit Headers

Every tool call response includes these headers:

| Header                  | Description                                    |
|-------------------------|------------------------------------------------|
| `X-RateLimit-Limit`     | Maximum requests allowed in the current window |
| `X-RateLimit-Remaining` | Requests remaining in the current window       |
| `X-RateLimit-Reset`     | Unix timestamp when the window resets          |
| `Retry-After`           | Seconds until next request is allowed (on 429) |

---

## SSE Transport

The MCP service uses Server-Sent Events (SSE) as its transport layer, following the MCP SSE specification.

### Connection Flow

1. **Open SSE stream** — `GET /mcp/sse` with the `Authorization` header. The server responds with `Content-Type: text/event-stream`.
2. **Receive endpoint event** — the first SSE event contains the message endpoint URL with a `sessionId` query parameter.
3. **Send JSON-RPC messages** — `POST /mcp/messages?sessionId={id}` with `Content-Type: application/json` and the same `Authorization` header.
4. **Receive responses** — tool call results arrive as SSE events on the open stream.

### Reconnection

If the SSE connection drops:

- Reopen a new `GET /mcp/sse` connection. A new session ID will be issued.
- Re-send any pending tool calls on the new session.
- Implement exponential backoff starting at 1 second, capping at 30 seconds.
- The server allows up to 2 concurrent SSE connections on free tier, 10 on pro tier.

### CORS

The server sends permissive CORS headers:

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, OPTIONS
Access-Control-Allow-Headers: Content-Type, Authorization
Access-Control-Expose-Headers: X-RateLimit-Limit, X-RateLimit-Remaining, X-RateLimit-Reset, Retry-After
```

---

## Building a Plugin

### VS Code Extension

1. **Scaffold the extension:**
   ```bash
   npx yo @anthropic-ai/create-mcp-extension toke-vscode
   cd toke-vscode
   npm install
   ```

2. **Configure MCP connection in `package.json`:**
   ```json
   {
     "contributes": {
       "mcpServers": {
         "toke-cloud": {
           "type": "sse",
           "url": "https://mcp.tokelang.dev/mcp/sse"
         }
       }
     }
   }
   ```

3. **Call tools from your extension code:**
   ```typescript
   import { Client } from "@modelcontextprotocol/sdk/client/index.js";
   import { SSEClientTransport } from "@modelcontextprotocol/sdk/client/sse.js";

   const transport = new SSEClientTransport(
     new URL("https://mcp.tokelang.dev/mcp/sse"),
     {
       requestInit: {
         headers: {
           Authorization: `Bearer ${apiKey}`,
         },
       },
     }
   );

   const client = new Client({ name: "toke-vscode", version: "0.1.0" });
   await client.connect(transport);

   const result = await client.callTool("toke_check", {
     source: editor.document.getText(),
   });
   ```

4. **Map diagnostics to VS Code problems:**
   Parse the JSON diagnostics from `toke_check` and convert `pos.line` / `pos.col` to `vscode.Diagnostic` objects on a `DiagnosticCollection`.

### JetBrains Plugin

1. **Create a new IntelliJ plugin project** using the IntelliJ Platform Plugin Template.

2. **Add the MCP client dependency** (Java/Kotlin MCP SDK or use raw HTTP + SSE).

3. **Register a `LocalInspectionTool`** that calls `toke_check` on file save and converts diagnostics to `ProblemDescriptor` objects.

4. **SSE in JetBrains:** Use `okhttp-sse` or `java.net.http.HttpClient` to open the SSE stream. Route JSON-RPC messages through the standard MCP message flow.

### Neovim Plugin

1. **Install the MCP Neovim client:**
   ```lua
   -- lazy.nvim
   { "mcp-neovim/mcp.nvim" }
   ```

2. **Configure the toke MCP server in your Neovim config:**
   ```lua
   require("mcp").setup({
     servers = {
       ["toke-cloud"] = {
         transport = "sse",
         url = "https://mcp.tokelang.dev/mcp/sse",
         headers = {
           Authorization = "Bearer " .. vim.env.TOKE_API_KEY,
         },
       },
     },
   })
   ```

3. **Create a check-on-save autocommand:**
   ```lua
   vim.api.nvim_create_autocmd("BufWritePost", {
     pattern = "*.tk",
     callback = function()
       local source = table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), "\n")
       require("mcp").call_tool("toke-cloud", "toke_check", { source = source }, function(result)
         -- Parse result and populate the quickfix list
       end)
     end,
   })
   ```

---

## Example Code

A minimal Node.js client that connects to the MCP service and runs `toke_check`:

```typescript
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { SSEClientTransport } from "@modelcontextprotocol/sdk/client/sse.js";

const API_KEY = process.env.TOKE_API_KEY || "";

async function main() {
  // 1. Create SSE transport
  const transport = new SSEClientTransport(
    new URL("https://mcp.tokelang.dev/mcp/sse"),
    {
      requestInit: {
        headers: {
          ...(API_KEY ? { Authorization: `Bearer ${API_KEY}` } : {}),
        },
      },
    }
  );

  // 2. Connect MCP client
  const client = new Client({ name: "toke-example", version: "0.1.0" });
  await client.connect(transport);
  console.log("Connected to toke MCP service");

  // 3. List available tools
  const tools = await client.listTools();
  console.log("Available tools:", tools.tools.map((t) => t.name));

  // 4. Check some toke source code
  const source = 'm=hello;\nf=main():i64{<42};';
  const checkResult = await client.callTool("toke_check", { source });
  console.log("Check result:", checkResult.content[0].text);

  // 5. Explain an error code
  const explainResult = await client.callTool("toke_explain_error", {
    error_code: "E2001",
  });
  console.log("Explain result:", explainResult.content[0].text);

  // 6. Look up a stdlib module
  const stdlibResult = await client.callTool("toke_stdlib_ref", {
    module: "str",
  });
  console.log("Stdlib result:", stdlibResult.content[0].text);

  // 7. Clean up
  await client.close();
}

main().catch(console.error);
```

### Running the Example

```bash
npm install @modelcontextprotocol/sdk
export TOKE_API_KEY="toke_free_your_key_here"
npx tsx example.ts
```

---

## Testing

### Staging Environment

Before publishing your plugin, test it against the staging environment:

```
https://mcp-staging.tokelang.dev/mcp/sse
```

The staging server runs the same MCP tools with the same schemas but uses a separate rate limit pool. Staging API keys use the same format and can be obtained from the developer dashboard.

### Health Check

Verify the server is running before connecting:

```bash
curl https://mcp-staging.tokelang.dev/health
```

Expected response:

```json
{ "status": "ok", "version": "0.1.0", "tkc": true }
```

### Test Checklist

- [ ] SSE connection opens successfully with your API key
- [ ] `tools/list` returns all 7 tools (5 on free tier, 7 on pro tier)
- [ ] `toke_check` returns valid diagnostics for both correct and incorrect toke source
- [ ] `toke_compile` returns LLVM IR for valid programs
- [ ] `toke_explain_error` returns explanations for known codes (E1001, E2001, E4031) and graceful "not found" for unknown codes
- [ ] `toke_spec_lookup` returns relevant sections for queries like `arrays`, `keywords`, `types`
- [ ] `toke_stdlib_ref` returns module overviews and individual function details
- [ ] Rate limit headers are present on every response
- [ ] HTTP 429 is returned when rate limits are exceeded, with correct `Retry-After` header
- [ ] SSE reconnection works after a deliberate disconnect
- [ ] Your plugin handles network errors, timeouts, and malformed responses gracefully

### Automated Test Script

Run the MCP compatibility tests from the toke-cloud repository:

```bash
git clone https://github.com/tokelang/toke-cloud.git
cd toke-cloud
npm install
TOKE_MCP_URL="https://mcp-staging.tokelang.dev" node test/compatibility/mcp_compat_test.js
```