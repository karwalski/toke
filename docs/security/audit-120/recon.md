# Epic 120 Security Audit — Reconnaissance

Status: reconnaissance complete for the three lightly-explored in-scope repos.
Scope of this document: entry points, deployed artifacts, transports, and trust
boundaries. Findings/severities are tracked separately (see `README.md` and the
per-area reports).

> PUBLIC-REPO NOTE: this file lives in the public `toke` repo. It deliberately
> contains no IP addresses, hostnames, SSH usernames, or key/credential file
> paths. Infrastructure is referred to indirectly (e.g. "the managed cloud edge",
> "the public API endpoint", "the deploy host").

---

## 1. toke-mcp (public MCP server)

**Purpose.** Self-hostable Model Context Protocol server exposing the toke
toolchain to AI clients. Published as an npm package and as a self-hosted Docker
image. This is the consumer-facing counterpart to the private cloud service.

### Entry points

| Entry point | File | Role |
|---|---|---|
| npx launcher | `bin/toke-mcp.js` | CLI wrapper: resolves port + locates the `tkc` binary, then imports `server.js` (HTTP mode). |
| stdio transport | `bin/stdio.js` | Direct MCP-over-stdio for local editor/agent integration; builds server via `createMcpServer()` and connects a `StdioServerTransport`. |
| HTTP/SSE server | `server.js` | Exports `createMcpServer()` (tool registration) and `createApp()` (Express app). Self-starts an HTTP listener when run directly. |
| Lambda adapter | `lambda/mcp/index.js` | Minimal serverless SSE/JSON-RPC adapter; only routes `toke_check` and `toke_compile`. |

### Transports

- **stdio** — `StdioServerTransport` (local, in-process, no network).
- **HTTP + SSE** — Express app. `GET /mcp/sse` opens an SSE session,
  `POST /mcp/messages?sessionId=...` dispatches JSON-RPC tool calls. Also
  `GET /health` (runs `tkc --version`).
- Default listen port 3000. CORS is wide open (`Access-Control-Allow-Origin: *`,
  methods GET/POST/OPTIONS).

### Tools exposed (15, registered in `server.js`)

`toke_check`, `toke_compile`, `toke_explain_error`, `toke_spec_lookup`,
`toke_stdlib_ref`, `toke_generate`, `toke_bench`, `toke_companion`,
`toke_format`, `toke_migrate`, `toke_compress`, `toke_decompress`,
`toke_analyse`, `toke_render`, `toke_feedback`.

Tool behaviour classes relevant to the audit:
- **Invoke the `tkc` binary on client-supplied source** — `toke_check`
  (`--check --diag-json`), `toke_compile` (`--emit-llvm`), and the
  compress/decompress/format/migrate/bench family. Each writes the client's
  source to a temp file under the OS temp dir and calls `execFile(TKC_PATH, ...)`
  with a timeout. `execFile` (not a shell) is used, so there is no shell-string
  interpolation; the primary surface is `tkc` itself parsing/compiling untrusted
  input, plus temp-file handling.
- **Call an external network API** — `toke_generate` and `toke_feedback` POST to
  the public generate/feedback API endpoint via `lib/api-client.js`, sending an
  API key read from an environment variable. `toke_generate` then re-checks the
  returned source locally with `toke_check`.
- **Pure JS, no external process** — `toke_render`, `toke_analyse`,
  `toke_explain_error`, `toke_spec_lookup`, `toke_stdlib_ref`.

### Built-in protections

- Per-IP/per-tool sliding-window rate limiting and connection caps
  (`lib/rate-limit-middleware.cjs`, `lib/connection-registry.cjs`,
  `lib/rate-limiter.cjs`), with optional Redis backing (`ioredis` optional dep).
- Pluggable `onConnect` / `onToolCall` / `onToolComplete` hooks let a wrapper
  (the private cloud server) inject auth, tiering, and usage tracking.

### Deployed artifacts

- npm package `@tokelang/mcp-server` (public).
- `Dockerfile.selfhosted` — multi-stage node:20-slim image; bundles the `tkc`
  binary at `/usr/local/bin/tkc`, runs as the non-root `node` user, health check
  via curl. `docker-compose.yml` publishes port 3000.
- A bare `lambda/` variant (check/compile/mcp handlers) for serverless hosting.

### Trust boundaries

- **Untrusted MCP client → `tkc` execution.** In the default/self-hosted path
  there is **no auth and no sandbox**: any client that can reach the SSE endpoint
  can drive the local `tkc` binary with arbitrary source. Sandboxing exists only
  in the private cloud wrapper (see §3), not in this package.
- **Server → external API** over the network for generate/feedback, gated by an
  env-var API key.
- Wide-open CORS means browser-origin callers are not restricted at that layer.

---

## 2. toke-website (tokelang.dev)

**Purpose.** The public website/docs for toke, built with the `ooke` native web
framework and compiled to a single self-contained binary that serves the site
over HTTPS.

### Entry points / source

- `main.tk` — the ooke application source (module `website`). Registers routes:
  static health JSON at `/health`, `/api/health`, `/api/version`; serves the
  rendered `build/` tree; renders `templates/` per request via
  `http.servepages`; and configures virtual hosts for the primary, `www`,
  `staging`, and `loke` subdomains.
- `main.ll` — LLVM IR emitted from `main.tk` (build intermediate).
- `ooke.toml` — site config: build output `build/`, minify on; server port 8081,
  `admin = false`, `workers = 0`; `flat` store backend; combined access logs
  (max 10000 lines / 30 days).

### Deployed artifact(s)

- `website` — the compiled native server binary (the deployed artifact).
- Additional binaries present in-tree: `ooke-toke-pure` (the pure-toke ooke
  rebuild), `main`, and `dev-server` (local dev). `main.tkc` companion present.

### Transport / what gets served

- **Default: HTTPS** — `http.servevhoststls(port, certs/cert.pem, certs/key.pem)`
  binds the TLS listener (default port 443 in `main.tk`; `ooke.toml` lists 8081
  for the plain listen mode). `--http`/`--port` flags switch to plain HTTP with
  worker processes.
- Serves: static rendered HTML (`build/`), dynamically rendered templates
  (`templates/`, read per-request), vhost site roots (`sites/`), and static
  assets (`static/`, incl. tokenizer data). Health/version endpoints return
  static JSON.

### Build & deploy

- `Makefile` — emits IR with the local toke compiler, then links the ooke stdlib
  C sources + vendored cmark/tomlc99 with clang (OpenSSL, sqlite3, zlib). Also
  provides `check-docs*` targets that compile and run doc examples, and a
  self-signed cert generator.
- `scripts/deploy.sh` — deploys to a remote host over SSH/rsync in `content`,
  `full`, or `auto` mode. `full` mode: emit IR locally → rsync IR + stdlib C
  sources to the deploy host → compile the native binary **on the server** with
  clang → `pkill`/`nohup` restart → `/health` smoke test. `content` mode rsyncs
  `build/`, `sites/`, `certs/`, `static/`, `templates/` with `--delete` and no
  restart. Host, SSH user, key, and remote dir come from environment variables.

### Trust boundaries

- **Public internet → website binary.** The binary terminates TLS and serves
  static + per-request template-rendered content; template rendering is the main
  server-side input-handling surface to review.
- **Developer workstation → deploy host** over SSH. The deploy script uses
  `StrictHostKeyChecking=no` and ships C source to be compiled server-side, so
  the deploy channel and the server-side build step are part of the trust chain.
- Note for reviewers: `scripts/deploy.sh` and the `Makefile` contain concrete
  infra details (a host address, a key path, subject-alt-name domains). Those are
  intentionally **not** reproduced here per the public-repo rule.

---

## 3. toke-cloud (private)

**Purpose.** Private infrastructure repo powering the managed MCP service:
deployment automation, auth, billing, rate limiting, monitoring, and a sandboxed
execution environment. It **wraps** the public `@tokelang/mcp-server` and adds
the paid/managed layer.

### Entry point

- `server.js` — imports `createApp()` from `@tokelang/mcp-server` and injects
  private middleware via the hooks:
  - `onConnect` → API-key authentication (`lib/auth-middleware.js`) + tier-based
    connection limits.
  - `onToolCall` → tiered rate limiting (`lib/rate-limit-middleware.js`,
    `lib/tier-limits.js`).
  - `onToolComplete` → usage tracking for analytics/billing
    (`lib/usage-tracker.js`).
  - Serves the managed MCP domain; the console/billing runs as a separate
    surface.

### Components / what runs where

| Area | Contents | Runs where |
|---|---|---|
| `infra/` | AWS CDK (TypeScript) stacks: API Gateway, Lambda, Fargate sandbox, CloudFront, WAF, Redis cache, SageMaker endpoint, DynamoDB, billing, monitoring alarms, multi-region, telemetry pipeline. | AWS, deployed via `cdk deploy`. |
| `lambda/` | Handlers: `api-gateway`, `billing`, `check`, `compile`, `mcp`, `telemetry-ingest`. | AWS Lambda. |
| `lib/` | `api-keys`, `auth-middleware`, `rate-limit-middleware`, `stripe-billing`, `telemetry`, `tier-gate`, `tier-limits`, `usage-tracker`, `sagemaker-client`. | Shared, in Lambda / container. |
| `console/` | `admin.html`, `index.html`, `api.js` — internal web UI for API-key management. | Static console app + API. |
| `sandbox/` | Hardened Fargate container: `Dockerfile`, `entrypoint.sh`, `seccomp-profile.json`, `test_security.sh`. Read-only filesystem, no network, ~5s hard kill. | AWS Fargate. |
| `model/` | `serving_config.json` — model serving config. | Backs the generate tool via a managed model endpoint. |
| root | `server.js` (local dev), `Dockerfile` (prod container, bundles `tkc`, port 3000), `.gitleaks.toml`. | Local / container. |

### Transports

- Same MCP HTTP+SSE contract as the public server (inherited from
  `createApp()`), fronted at the managed edge (CloudFront + WAF + API Gateway).
- Serverless `lambda/mcp` provides an alternate SSE/JSON-RPC surface exposing
  `toke_check` and `toke_compile`.

### Trust boundaries (defence in depth)

1. **remote-unauth edge** — CloudFront + WAF + API Gateway terminate and filter
   public traffic.
2. **auth** — API-key validation (`auth-middleware`) gates connections; tiers
   (free/pro) drive rate and connection limits.
3. **execution isolation** — untrusted source is compiled/run inside the
   seccomp-profiled Fargate sandbox (read-only FS, no network, hard timeout),
   not on the API host.
4. **billing/state** — Stripe integration and DynamoDB key storage; Redis for
   rate-limit state; all secrets via the cloud secrets manager (none committed;
   `.gitleaks.toml` present).
5. **model backend** — the generate path calls a managed model endpoint via
   `sagemaker-client`.

Key review question for this repo: whether the sandbox is actually on the path
for **every** tkc-invoking tool (including the Lambda `check`/`compile`
handlers), or whether some tool paths compile untrusted source outside the
Fargate isolation.

---

## Cross-repo summary

- The **only hardened execution isolation** for `tkc` on untrusted input lives in
  toke-cloud's Fargate sandbox. Both toke-mcp (self-hosted default) and the
  Lambda check/compile handlers invoke `tkc` on client input without that
  sandbox — a primary theme for the audit.
- toke-cloud is a thin auth/billing/rate-limit wrapper around the public
  toke-mcp `createApp()`, so **any weakness in the shared `createApp()`/tool
  layer is inherited by the managed service.**
- toke-website is a distinct native artifact (ooke binary) with its own
  TLS-terminating server, per-request template rendering, and an SSH/rsync +
  compile-on-server deploy pipeline.
