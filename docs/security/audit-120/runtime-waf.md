## 120.13 — In-runtime defense review (WAF layer)

**Scope:** `src/stdlib/security.c` (561 lines) — the module Epic 65 designates as the
in-runtime defense: token-bucket rate limiting, per-route limits, per-IP connection
limits, Slowloris params, URI/body validation, SQLi/XSS heuristics, a WAF rule engine,
and CSP/CORS builders. **Method:** static + manual review of the module and its call
sites. I traced every exported `security_*` symbol across the whole `http` module set
(`http.c http2.c acme.c proxy.c cache.c content.c metrics.c server_ops.c ws_server.c
hooks.c tk_web_glue.c` — the C files `stdlib_deps.c:61` links for an `i= http` import),
the request-handling path, the glue layer, and the build graph. No build/compile was run
(shared work-tree). The dominant finding is that this "only in-runtime defense" is linked
but **never invoked**; the remaining findings are latent defects that become live the
moment it is wired in, and they directly inform the ADR-0011 default-on recommendation.

---

### RUN-01 — The entire WAF / rate-limit / SQLi / XSS / URI-validation engine is dead code: linked into every http program but never invoked and not reachable from toke
- **Severity:** High
- **Reachability:** build-time (design gap — the control is absent at runtime)
- **File:** `src/stdlib/security.c:1-561`; call-site evidence `src/stdlib/http.c` (0 calls), `src/stdlib/tk_web_glue.c` (0 calls), `src/stdlib_deps.c:61`
- **Description:** `security.c` is compiled and linked into every toke program that imports `http` (it is in the `http` module C-file list at `stdlib_deps.c:61`), but nothing calls it. Grepping every exported entry point (`security_rate_check`, `security_check_sqli`, `security_check_xss`, `security_waf_check`, `security_validate_uri`, `security_conn_add`, `security_route_rate_check`, `security_check_json_depth`, `security_csp_build`, `security_cors_find`, `security_get_header_timeout`, `security_get_auth_endpoint`, `security_client_cert_required`) across the full http module set yields **zero call sites outside `security.c` itself**. The request path in `http.c` emits `write_security_headers()` (a separate, self-contained function in `http.c`) but never performs rate limiting, connection limiting, Slowloris enforcement, URI validation, SQLi/XSS screening, or WAF evaluation. Furthermore, none of these functions are `tk_*`-prefixed and there is **no `security_glue.c`**, so `scripts/gen_stdlib_decls.py` (which only exports `tk_*` functions, per its header comment) never surfaces them to toke — an ooke application cannot even opt in to call them. The `security_set_*` configuration setters (rate, route limits, conn limits, Slowloris, body limits, CSP, CORS, auth endpoint, client cert) exist with no enforcement hook consuming them. `g_max_form_fields` (set by `security_set_body_limits`) has no checker at all; the Slowloris params (`security_set_slowloris_params`, `security_get_header_timeout`) are stored and never read by the accept/read loop.
- **Impact:** The story's premise — "this is the ONLY in-runtime defense" — evaluates to **zero runtime protection**. A deployed ooke/http server has no rate limiting, no connection cap, no Slowloris mitigation, and no request-content screening from this module. The presence of the file creates a dangerous false sense of security in the threat model.
- **Recommended fix:** Wire the engine into the `http.c` request path *before* dispatch: (1) call `security_conn_add`/`security_conn_remove` around connection lifetime; (2) call `security_rate_check`/`security_route_rate_check` per request, emitting 429 + `Retry-After`; (3) enforce the header read timeout / min-rate in the accept-read loop; (4) run `security_validate_uri` and (if applicable) `security_check_json_depth` before body processing; (5) evaluate `security_waf_check`. Decide default-on vs opt-in per ADR-0011 (recommendation below). Also add `tk_*` glue so applications can configure it. Until wired, mark the module explicitly experimental/non-functional in the guide so it is not cited as a control.

---

### RUN-02 — Rate limiter and connection limiter fail *open* when the bucket table is exhausted
- **Severity:** Medium (contingent on RUN-01 wiring)
- **Reachability:** remote-unauth (once wired)
- **File:** `src/stdlib/security.c:62,76` (rate), `:162` (conns)
- **Description:** `rate_find_or_create` returns `NULL` when `g_rate_bucket_count >= RATE_MAX_BUCKETS` (4096) (`:62`). Both `security_rate_check` (`:76 return 1`) and `security_conn_add` (`:162 return 1`) treat a `NULL` bucket as **allow**. Cleanup only evicts entries idle for >300s (`:49`), so an attacker who sources requests from ≥4096 distinct addresses (trivial over IPv6, or a botnet) pins the table full; every subsequent request — from *any* IP, including new attack traffic — is then unconditionally allowed with no rate or connection limiting.
- **Impact:** Global rate-limit and connection-limit bypass under a modest distributed load; the defense is weakest exactly when under attack.
- **Recommended fix:** Fail *closed* (or into a degraded global limiter) when the table is full; use an LRU/least-tokens eviction instead of only age-based cleanup; size the table for expected peak client cardinality and/or hash into fixed shards so an unknown IP still maps to a bucket.

---

### RUN-03 — Pre-fork worker model multiplies limits and de-globalizes connection accounting
- **Severity:** Medium
- **Reachability:** remote-unauth (once wired)
- **File:** `src/stdlib/security.c:29-34,152,159-176`; server model `src/stdlib/http.c:1085,1229,1296,1366,4225` (`fork()` pre-fork pool)
- **Description:** All limiter state is `static` process-global (`g_rate_buckets`, `g_rate_bucket_count`, `g_max_conns_per_ip`, per-IP `active_conns`). The HTTP server is a **pre-fork worker pool** — each worker is a separate `fork()`ed process with its own private copy of this state. With *N* workers, the effective per-IP rate ceiling is *N × g_rate_limit_rps* and the effective connection cap is *N × g_max_conns_per_ip*, because a client's requests are load-balanced across workers that don't share buckets. Connection counts (`active_conns`) are per-worker and never reconciled, so `g_max_conns_per_ip` is not a global cap.
- **Impact:** Configured limits are silently exceeded by the worker multiple; connection limiting is per-worker, not per-IP, defeating its DoS-mitigation purpose.
- **Recommended fix:** Back the buckets with shared memory (`mmap(MAP_SHARED)` / atomics) or a small IPC-backed limiter shared across workers; or centralize limiting in the parent/accept stage before hand-off. Document the multiplier if a shared store is out of scope.

---

### RUN-04 — Per-route composite bucket key is truncated to 63 bytes, causing bucket collisions
- **Severity:** Medium
- **Reachability:** remote-unauth (once wired)
- **File:** `src/stdlib/security.c:133-135` (build `key[256]`), consumed at `:66` (`strncpy(b->ip, ip, sizeof(b->ip)-1)`, `ip[64]`)
- **Description:** `security_route_rate_check` builds a composite key `"<ip>|<pattern>"` in a 256-byte buffer (`:134`) and passes it to `security_rate_check` → `rate_find_or_create`, which stores it in `RateBucket.ip`, a **64-byte** field (`:21`), via `strncpy(..., sizeof(b->ip)-1)` (`:66`). Any key longer than 63 bytes is truncated. Long route patterns (or long/IPv6 client addresses) collide: distinct `(IP, route)` pairs sharing a 63-byte prefix share one token bucket. Legitimate clients on one route can then exhaust the bucket used by another route/IP, and an attacker can pick a route prefix that collides to either evade or amplify limiting.
- **Impact:** Incorrect per-route limiting — both false-positive throttling and bypass depending on prefix collisions.
- **Recommended fix:** Store a hash of the full key (e.g. 64-bit FNV/SipHash) rather than the raw string, or widen the key field and compare full keys; never silently truncate an identity used for a security decision.

---

### RUN-05 — SQL-injection heuristic is case-sensitive and trivially bypassable
- **Severity:** Medium
- **Reachability:** remote-unauth (once wired)
- **File:** `src/stdlib/security.c:269-289`
- **Description:** `security_check_sqli` uses **`strstr` (case-sensitive)** against a fixed pattern table that hard-codes only specific casings (`"' OR '"`, `"' or '"`, `"UNION SELECT"`, `"union select"`, etc.). Note the inconsistency with `security_check_xss`, which correctly uses `strcasestr`. Bypasses are one-liners: mixed case `' Or '1'='1`; alternate whitespace `UNION/**/SELECT`, `UNION  SELECT`, newline-separated tokens; no-space forms `'or'1'='1` (the table requires the spaced `' OR '`); MySQL comment `'#` (only `' --`/`';--` listed); and, because the check runs on the raw (non-URL-decoded) request, any percent-encoded payload (`%27%20OR%20`). Whole classes are absent (`information_schema`, stacked `; UPDATE`, `OR 1` without quotes, hex literals `0x...`, time-based via `pg_sleep`).
- **Impact:** High false-negative rate; if presented as *the* injection defense it is misleading. As a blocklist it also invites false positives on legitimate input.
- **Recommended fix:** Do not position this as a primary defense. Make it advisory/log-only by default and drive users to parameterized queries (the real fix). At minimum switch to `strcasestr`, normalize whitespace/comments, and decode before matching — while documenting that heuristic WAF matching is best-effort.

---

### RUN-06 — XSS heuristic is incomplete and matches pre-decode input
- **Severity:** Medium
- **Reachability:** remote-unauth (once wired)
- **File:** `src/stdlib/security.c:293-313`
- **Description:** `security_check_xss` (correctly `strcasestr`) matches a short literal list. Bypasses: event handlers not enumerated (`onmouseenter`, `onpointerdown`, `ontoggle`, `onanimationstart`, `onbeforeinput`, …); whitespace/encoding between attribute and `=` (`onerror =`, `onerror%3d`, `onerror&#x20;=`); vectors with no listed token (`<img src=x onerror=...>` is caught, but `<details ontoggle=...>`, `<math>`, `<base href=javascript:...>`, `data:` URIs, SVG animate are not); and, as with SQLi, the check runs on **non-decoded** input so `%3Cscript%3E`, entity-encoded, or fragmented payloads slip through.
- **Impact:** High false-negative rate for reflected/stored XSS screening; unsuitable as a primary XSS control.
- **Recommended fix:** Treat as advisory; the durable fix is context-aware output encoding / templating auto-escaping (see the template module) plus a strong CSP. If retained, decode+normalize before matching and expand the handler/tag set, accepting it remains best-effort.

---

### RUN-07 — CSP builder can overflow its buffer via unchecked `snprintf` accumulation
- **Severity:** Medium
- **Reachability:** local (values are set by the application via `security_csp_set`, not by the remote client)
- **File:** `src/stdlib/security.c:452-483` (esp. macro `:459-463`)
- **Description:** `security_csp_build` accumulates directives into a fixed 2048-byte buffer with `off += snprintf(buf + off, cap - (size_t)off, ...)`. `snprintf` returns the length it *would* have written, so once the total exceeds `cap`, `off` (an `int`) grows past `cap`; the next iteration computes `cap - (size_t)off`, which **underflows to a huge `size_t`**, and writes at `buf + off` — **past the end of the allocation**. Thirteen directives whose combined values exceed ~2 KB (e.g. long `img-src`/`connect-src` allowlists) trigger an out-of-bounds write.
- **Impact:** Heap buffer overflow / memory corruption. Not remote-attacker-controlled in the common case (CSP values come from app config), so severity is Medium, but it is a genuine memory-safety bug.
- **Recommended fix:** Clamp: after each `snprintf`, check the return against remaining capacity and stop/`realloc` if it would truncate; never let `off` exceed `cap`. Grow the buffer dynamically or bound each directive length.

---

### RUN-08 — WAF header matching dereferences unchecked values; REDIRECT action is unusable; substring-only matching is bypassable
- **Severity:** Low
- **Reachability:** remote-unauth (once wired) / build-time
- **File:** `src/stdlib/security.c:346-404` (header loop `:386-393`, redirect `:329,336,356`)
- **Description:** In `security_waf_check`, the `WAF_MATCH_HEADER` branch calls `strstr(hdr_values[h], r->pattern)` (`:387`) with no NULL guard; a NULL header value (some parsers store bare/valueless headers as NULL) causes a NULL-deref crash — a potential remote DoS depending on the caller's header representation. Separately, `WAF_ACTION_REDIRECT` is defined (`:329`) and `WafRule.redirect_url` exists (`:336`), but `security_add_waf_rule` always sets `redirect_url = NULL` (`:356`) with no setter — the redirect action can never carry a target. Matching is plain `strstr` substring only (no anchoring/regex), and an early `WAF_ACTION_ALLOW` rule short-circuits and can be crafted to whitelist-bypass later DENY rules.
- **Impact:** Possible crash on malformed headers; a documented action that silently cannot work; easy pattern evasion / accidental allow-listing.
- **Recommended fix:** Null-check `hdr_values[h]`; add a redirect-URL parameter or remove the REDIRECT action; document ordering semantics and consider anchored/regex matching.

---

### RUN-09 — Connection counter leaks and drifts, risking self-inflicted lockout
- **Severity:** Low
- **Reachability:** remote-unauth (once wired)
- **File:** `src/stdlib/security.c:159-176` (`conn_add`/`conn_remove`), cleanup `:46-56`
- **Description:** `security_conn_add` increments `active_conns` **before** the limit test and returns the over-limit verdict (`:163-164`), so even rejected connections increment the counter; if a caller drops an over-limit connection without a paired `security_conn_remove`, the count only rises. Independently, the periodic cleanup (`:48-54`) evicts any bucket idle >300s by `last_check`, which is only updated on rate checks — a long-lived connection (WebSocket/SSE) with `active_conns > 0` but no recent rate check can have its bucket evicted; the later `conn_remove` then finds no bucket and the count is simply lost, so accounting drifts.
- **Impact:** Over time an IP can be permanently locked out (counter never returns to 0) or under-counted (evicted mid-connection), making the connection cap unreliable.
- **Recommended fix:** Only count *accepted* connections (or decrement on rejection); never evict buckets with `active_conns > 0`; update liveness on connection activity, not just rate checks.

---

### RUN-10 — URI validator does not decode and misses traversal / double-encoding; permits raw TAB
- **Severity:** Low
- **Reachability:** remote-unauth (once wired)
- **File:** `src/stdlib/security.c:202-229`
- **Description:** `security_validate_uri` checks length, control chars, and single-level percent-encoding well (the `%00` and `i+2 >= len` bounds checks are correct). But it validates the **raw** string only: it blocks literal `%00` (`:224`) yet not double-encoded `%2500`, does not reject `..` path traversal or backslashes, and explicitly allows TAB (`0x09`) inside a URI (`:214`), which can aid request-smuggling/normalization mismatches with downstream proxies.
- **Impact:** Encoded traversal and smuggling-adjacent inputs pass validation; modest given decoding/routing happens elsewhere, but a defense-in-depth gap.
- **Recommended fix:** Reject raw TAB in the request target; optionally decode once and re-scan for `%`, control bytes, and `../` before routing; document that URI validation is structural, not a traversal control.

---

### RUN-11 — Process-wide singletons and shared-global mutation in the route check
- **Severity:** Info
- **Reachability:** local
- **File:** `src/stdlib/security.c:424` (single `g_csp`), `:137-143` (mutates `g_rate_limit_rps`/`g_rate_burst` then restores)
- **Description:** CSP is a single process-wide `CspPolicy g_csp` — there is no per-route CSP even though per-route CORS exists, so a multi-tenant/per-route CSP is impossible. `security_route_rate_check` temporarily overwrites the global rate/burst, calls `security_rate_check`, then restores (`:137-143`); this is safe under the current one-connection-at-a-time-per-worker model but is fragile and would race if the server ever adds threads within a worker.
- **Impact:** Limited today (pre-fork, single-threaded per worker); a maintainability/robustness hazard and a functional CSP limitation.
- **Recommended fix:** Make CSP per-route like CORS; pass rate/burst as parameters instead of mutating globals.

---

## Default-on recommendation (feeds ADR-0011)

Given RUN-01, the first decision is simply *to invoke the module at all*. Recommended posture once wired:

- **Default-ON (safe, low false-positive):** URI length/structure validation (`security_validate_uri`), JSON depth limit, per-IP connection cap, Slowloris header timeout/min-rate, and a conservative global rate limit. These are structural/resource controls with negligible false positives.
- **Default-ON, response-only:** `write_security_headers` is already emitted on every response — keep it. Add a sane default CSP/`frame-ancestors` opt-in.
- **Default-OFF / advisory (log-only) unless explicitly enabled:** the SQLi and XSS blocklist heuristics (RUN-05/06). They are best-effort, easily bypassed, and prone to false positives; blocking on them by default would both break legitimate requests and lull developers away from parameterized queries and output encoding, which must remain the primary defenses.
- **Opt-in:** custom WAF rules, per-route rate limits, per-route CORS, sub-request auth, client certs — these are policy choices.

Fix RUN-02 (fail-closed), RUN-03 (shared-memory limiter), and RUN-04 (key truncation) **before** any rate/connection limit is advertised as effective, otherwise the default-on limiters give false assurance.

---

## Dynamic-testing follow-up (not run — shared work-tree)

- **ASAN load test of `security_csp_build` (RUN-07):** configure long values across many directives (>2 KB total) and confirm the heap OOB write; validate the fix under ASAN.
- **Fuzz `security_validate_uri`, `security_check_json_depth`, `security_check_sqli`, `security_check_xss`** with libFuzzer + ASAN — targets already exist for http parsing (`make fuzz-http`); add harnesses for these entry points to check for the string-skip loop in `check_json_depth` (`:254-260`) and pattern scanning on adversarial/truncated input.
- **Rate-limiter exhaustion test (RUN-02):** once wired, drive ≥4096 distinct source IPs and confirm fail-open, then confirm the fix fails closed.
- **Pre-fork multiplier test (RUN-03):** measure effective RPS/connection ceiling with N>1 workers against the configured value.
- **Route-key collision test (RUN-04):** exercise long IPv6 + long route prefixes and confirm bucket collisions.

## Positive observations

- `security_validate_uri` percent-encoding bounds are correct: the `i + 2 >= len` guard (`:220`) has no off-by-one, and it rejects encoded NUL `%00` (`:224`) and raw NUL/control bytes (`:211-214`).
- `security_check_xss` correctly uses `strcasestr` for case-insensitive matching (`:310`).
- `security_check_json_depth` skips string contents and honors backslash escapes (`:254-260`), so braces/brackets inside JSON strings don't inflate the depth count.
- Client IP is derived from `getpeername` (`http.c:558-571`), not from a client-controlled `X-Forwarded-For`, so per-IP identity is not header-spoofable at the socket layer (behind a proxy is a separate deployment concern).
- `security_csp_set` frees the previous value before `strdup` (`:446-447`), avoiding a leak on repeated sets, and `security_cors_find` uses correct longest-prefix matching (`:519-529`).
- Rate-bucket cleanup compacts the array in place (`:47-54`) without leaking `RateBucket` entries (they are value structs, not heap pointers).
