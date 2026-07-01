## 120.15 — toke-mcp audit (public MCP server)

**Scope / method.** Static, read-only review of the public `toke-mcp` repo (`/Users/matthew.watt/tk/toke-mcp`): the SSE/HTTP entry point (`server.js`), the stdio entry point (`bin/`), all MCP tool handlers (`tools/*.js`), the shared libraries (`lib/*.cjs`, `lib/*.js`), and the AWS Lambda variants (`lambda/*`). I focused on MCP message input validation, tool authentication/authorization, the `tkc` compiler/runtime execution surface (argument injection, resource limits), rate limiting, and secret handling. No build system, `tkc`, or long-running command was executed; findings are grounded in specific lines I read.

The server is intentionally an **unauthenticated public/self-hostable** MCP server that shells out to the native `tkc` compiler (a C binary) once per tool call. That design is acceptable in principle, but its only compensating controls are per-IP rate limiting and per-IP connection limits — and those controls do not actually work in the shipped code.

---

### TOK-01 — Rate limiting and connection limits silently fail open (advertised control is non-functional)
- **Severity:** High
- **Reachability:** remote-unauth
- **Location:** `lib/redis-client.cjs:10-25`, `lib/rate-limiter.cjs:39-72`, `lib/connection-registry.cjs:45-59,134-155`, wired from `server.js:325-328,376-382`
- **Description.** `lib/redis-client.cjs` is the only Redis client shipped in the repo. `getClient()` (lines 10-25) returns an in-memory stub that implements **only** `get / set / incr / expire / del / quit`. It never reads `REDIS_URL` and never connects to a real Redis. However, the rate limiter and connection registry are built on Redis **sorted-set** commands that the stub does not implement: `rate-limiter.cjs:41-47` calls `client.multi().zRemRangeByScore().zAdd().zCard().expire().exec()`, and `connection-registry.cjs:134-151` calls `client.zRemRangeByScore()` / `client.zCard()`. Because `client.multi` (and `zAdd`, `zCard`, etc.) are `undefined` on the stub, the very first call throws a `TypeError`, which is caught by the surrounding `try/catch` that is coded to **fail open**: `rate-limiter.cjs:69-72` returns `{ allowed: true }`, and `connection-registry.cjs:152-155` returns count `0`. Net effect: every tool call is allowed and every new SSE connection is allowed, regardless of volume.
- **Impact.** On any self-hosted deployment of this package (the README, line 12/124-126, explicitly advertises "Rate limiting with in-memory or Redis-backed storage" and an "in-memory fallback"), there is in practice **no rate limiting and no connection cap**. An unauthenticated remote client can issue unbounded `toke_compile` / `toke_check` / `toke_compress` calls, each of which spawns a `tkc` process (`tools/compile.js:23`, `tools/check.js:23`) and writes a temp file. This is a straightforward resource-exhaustion / denial-of-service vector (CPU, memory, PIDs, temp-disk) against the host, and unlimited concurrent SSE sessions. Worse, the operator is given false assurance because the control is documented as present.
- **Recommended fix.** Ship a real Redis client (the `ioredis` optional dependency is declared but never used) that honours `REDIS_URL`, or implement the sliding-window / connection-count logic in the in-memory stub using plain data structures so the "in-memory fallback" actually enforces limits. Critically, change the failure mode: for a public server, a rate-limiter backend error should **fail closed** (or degrade to a conservative local in-process limiter), not allow unlimited traffic. Add an integration test that asserts the Nth+1 request in a window is rejected using the shipped default client.

---

### TOK-02 — Native `tkc` compiler executed with no resource isolation or sandbox
- **Severity:** Medium
- **Reachability:** remote-unauth
- **Location:** `tools/compile.js:17-30`, `tools/check.js:17-27`, `tools/compress.js:22-33`, `tools/analyse.js:59-73`, `tools/companion.js:20-67`, `tools/migrate.js:16-26`
- **Description.** Every compiler-backed tool takes untrusted MCP `source`/`input`, writes it to a temp file, and runs `tkc` via `execFile` under the same UID/privileges as the Node server. The only guard is a wall-clock `timeout` (10s for compile/check/format/companion/migrate, 30s for compress/analyse). There is **no memory cap, no CPU/cgroup limit, no separate unprivileged user, no chroot/container boundary, and no cap on the number of concurrent `tkc` processes**. Since `tkc` is a C binary and there is no language-level sandbox (per audit context), a single crafted input that makes the compiler allocate heavily or spin can consume host memory for up to the timeout window; multiplied by the missing rate limit (TOK-01) this is trivially amplified. The non-Lambda handlers also do **not** enforce the 64 KB source cap that the Lambda handlers apply (`lambda/compile/index.js:24`, `lambda/check/index.js:24`); they rely solely on Express's default 100 KB JSON body limit.
- **Impact.** Remote-unauthenticated memory/CPU exhaustion of the host, and exposure of any `tkc` memory-safety bug directly to the process running the MCP server (which, per the ecosystem's ambient-authority model, has full OS access). A `tkc` crash takes down the server; a `tkc` OOM can take down the host.
- **Recommended fix.** Run `tkc` under an OS sandbox with hard memory/CPU limits (e.g. a `ulimit -v` wrapper, cgroups, a dedicated low-privilege user, or a container/`bwrap`/`firejail` jail). Bound the number of concurrent compiler processes with a semaphore/queue. Enforce an explicit source-size limit in the tool handlers (mirror the Lambda 64 KB cap) rather than depending on the transport's body limit.

---

### TOK-03 — Argument injection into `tkc` via `preserve_atoms` (no `--` end-of-options sentinel, no validation)
- **Severity:** Low
- **Reachability:** remote-unauth
- **Location:** `tools/compress.js:28-33`
- **Description.** `toke_compress` forwards the caller-controlled `preserve_atoms` array straight into the `tkc` argv: `args.push("--preserve-atom", pattern)` for each element, with no validation and no `--` sentinel to terminate option parsing. `execFile` (no shell) prevents classic shell injection, but attacker-supplied tokens still reach `tkc`'s own argument parser. Depending on how `tkc` parses options, values beginning with `-`/`--` can be misinterpreted as additional flags (this is the standard "argument injection" class). The blast radius is entirely a function of `tkc`'s flag surface (e.g. any flag that writes an output file, changes the input path, enables an eval/plugin mode, or reads other files) — none of which is constrained here.
- **Impact.** Potentially influences `tkc` behaviour beyond the intended `--compress` operation (unknown-flag errors at minimum; file write/read or mode change if `tkc` exposes such flags). Confidence is speculative because it depends on the `tkc` argument grammar, which is out of scope for this repo.
- **Recommended fix.** Insert a `--` sentinel before the input file and reject any `preserve_atom` beginning with `-`, or pass patterns through a value syntax `tkc` guarantees to treat as data (e.g. `--preserve-atom=<pattern>`). Also bound the count/length of patterns.

---

### TOK-04 — `tokeBench` "pro tier" gate is a no-op; tier enforcement absent
- **Severity:** Low
- **Reachability:** remote-unauth
- **Location:** `lib/tier-gate.js:4-6`, `tools/bench.js:143-145`, `server.js:128-129`
- **Description.** `tools/bench.js:144` calls `requireTier(context, "pro")` to gate the benchmark tool behind a paid tier, but `lib/tier-gate.js` is a hard-coded no-op (`return;`). Independently, `server.js:128-129` invokes `tokeBench({ source, task_id })` **without** passing any `context`, so even a real implementation would receive `undefined`. The intended authorization control therefore does nothing in this codebase.
- **Impact.** Low in the public/self-hosted context (there are no tiers here; auth is stated to be handled upstream by API Gateway). The risk is that the presence of `requireTier(...)` reads as an enforced control during future changes, and any tier/authorization logic silently passes. It is a latent authorization gap if this code is deployed as the tier boundary.
- **Recommended fix.** Either remove the dead gate to avoid a false sense of enforcement, or implement `requireTier` and thread `context` from the transport (`server.js` tool callbacks) into `tokeBench`. Add a test that asserts a non-pro context is rejected.

---

### TOK-05 — Wildcard CORS with credentialed headers exposed
- **Severity:** Info
- **Reachability:** remote-unauth
- **Location:** `server.js:290-297`
- **Description.** The server sets `Access-Control-Allow-Origin: *` and allows the `Authorization` header for all routes. The default deployment has no auth and no cookies, so impact today is negligible. However, if a self-hoster layers token auth via `Authorization` (as the CORS config anticipates), the wildcard origin lets any web page issue cross-origin MCP requests; browsers won't auto-attach a bearer token, but the permissive policy still broadens the browser-reachable attack surface and defeats any origin allow-listing.
- **Recommended fix.** Restrict `Access-Control-Allow-Origin` to a configurable allow-list (default: none / same-origin) rather than `*`, especially on any deployment that adds authentication.

---

### Positive observations
- **No shell interpolation anywhere.** Every compiler invocation uses `execFile`/`execFileSync` with an argv array (`tools/*.js`, `lambda/*`, `bin/toke-mcp.js:70`), so classic OS command injection is not present.
- **Temp files use unpredictable names** (`randomUUID()` in `os.tmpdir()` / `/tmp`) and are cleaned up in `finally` blocks (`tools/compile.js:46-48`, etc.), avoiding predictable-path / symlink races and leak accumulation.
- **Compiler calls are time-bounded** (`timeout` on every `execFile`), and Lambda handlers additionally cap source size (64 KB) and output buffer (2 MB) and reset `HOME=/tmp` (`lambda/compile/index.js:24,93-94`).
- **Secret handling is reasonable:** `TOKE_API_KEY` is read from env only, sent as an `X-Api-Key` header, and never logged or echoed to clients (`lib/api-client.js:6-24`); when unset the generate/feedback tools degrade gracefully.
- **MCP inputs are schema-validated** via `zod` on the SSE/stdio path (`server.js:63-266`), and the Lambda dispatcher validates the JSON-RPC version and rejects unknown tools/methods (`lambda/mcp/index.js:92-165`).
- **Telemetry is opt-in and privacy-preserving:** disabled unless `TOKE_TELEMETRY=1`, strips string-literal contents before writing, and stores only local hashed/redacted source (`lib/telemetry.js:9-18,60-96`).

### Dynamic-testing follow-up
- **TOK-01:** Stand up the server with the default (shipped) client and confirm empirically that the (N+1)th request within a window and the (maxConn+1)th SSE connection are both *allowed* — i.e. reproduce the fail-open behaviour under load — then re-test after the fix. Also fuzz Redis-outage behaviour to verify the corrected fail-closed path.
- **TOK-02:** Under ASAN/valgrind and a memory-limited harness, fuzz `tkc` through `toke_compile`/`toke_check` with adversarial toke source to characterise peak memory/CPU per request and probe for compiler crashes reachable from the MCP surface (coordinate with the `tkc` compiler-audit story). Load-test concurrent compiles to quantify host exhaustion with no rate limit.
- **TOK-03:** Enumerate the real `tkc` flag grammar and test whether a `preserve_atom` value beginning with `-`/`--` is parsed as a flag (e.g. can it redirect output, change input, or toggle a mode). This determines whether TOK-03 is Low or should be escalated.
