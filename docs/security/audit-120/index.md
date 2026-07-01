# Epic 120 — Security Audit: Master Roll-Up Index

Consolidated index of every finding produced across Epic 120's per-module security
audit. Machine-readable source of record: the story reports listed under
[Per-module reports](#per-module-reports). This index is generated from the
consolidated findings list; do not hand-edit individual rows — update the
per-module report and regenerate.

- **Total findings:** 113 across 16 audited modules
- **High-severity findings:** 24 (each carries an adversarial-verification verdict: 16 CONFIRMED, 8 REFUTED)
- **Verified-exploitable High findings (CONFIRMED):** 16 — these are the immediate remediation priority

---

## Summary — counts by severity

| Severity | Count |
|---|---|
| High | 24 |
| Medium | 39 |
| Low | 42 |
| Info | 8 |
| **Total** | **113** |

## Summary — counts by reachability

| Reachability | Count |
|---|---|
| remote-unauth | 48 |
| remote-auth | 18 |
| local | 37 |
| build-time | 10 |
| **Total** | **113** |

## Summary — severity × reachability

| Severity | remote-unauth | remote-auth | local | build-time | Total |
|---|---|---|---|---|---|
| High | 12 | 5 | 3 | 4 | 24 |
| Medium | 18 | 7 | 11 | 3 | 39 |
| Low | 14 | 6 | 19 | 3 | 42 |
| Info | 4 | 0 | 4 | 0 | 8 |
| **Total** | **48** | **18** | **37** | **10** | **113** |

## High-severity adversarial-verification verdicts

Every High finding was independently re-checked by an adversarial verifier.

| Verdict | Count | Meaning |
|---|---|---|
| CONFIRMED | 16 | Exploitability reproduced/validated — fix required |
| REFUTED | 8 | Verifier could not substantiate the claim as stated — re-scope/downgrade before filing |

---

## All findings

Verdict column applies to High-severity findings only (— = not applicable).

| Story | ID | Severity | Reachability | Title | File:Line | Verdict |
|---|---|---|---|---|---|---|
| 120.2 | COM-01 | High | build-time | Build-time command injection via source filename / --out / --target interpolated into system() | `src/llvm.c:7424` | CONFIRMED |
| 120.2 | COM-02 | Medium | build-time | Path/command injection via untrusted TKC_STDLIB_DIR / TKC_RUNTIME_DIR environment variables | `src/llvm.c:7412` | — |
| 120.2 | COM-03 | Medium | local | Predictable temp-file name in auto-glue writer enables symlink overwrite and build-time C injection | `src/glue_gen.c:411` | — |
| 120.2 | COM-04 | Low | local | clang toolchain resolved via $PATH with no pinning or integrity check | `src/llvm.c:7424` | — |
| 120.3 | COM-01 | High | local | Heap buffer overflow in migrate prepass (unchecked o[w++] past slen*2+256) | `src/migrate.c:93` | CONFIRMED |
| 120.3 | COM-02 | Medium | local | Unbounded recursion in recursive-descent parser and AST-walking passes (stack exhaustion) | `src/parser.c:407` | — |
| 120.3 | COM-03 | Low | local | Integer overflow on int-typed length/size arithmetic for very large inputs | `src/migrate.c:93` | — |
| 120.4 | COM-01 | High | local | decompress_text() output can exceed the caller's len*4 buffer (heap overflow via back-references) | `src/compress.c:400` | CONFIRMED |
| 120.4 | COM-02 | Medium | local | compress_stream_feed() infinite loop on a token larger than the pending buffer (DoS) | `src/compress.c:898` | — |
| 120.4 | COM-03 | Low | local | Unvalidated ftell() result before malloc/fread in companion file readers | `src/companion.c:607` | — |
| 120.4 | COM-04 | Info | local | Signed-int index accumulation and dictionary truncation in decompress_text (robustness) | `src/compress.c:399` | — |
| 120.5 | PAR-01 | High | remote-unauth | yaml_from_json / yaml_to_json write past heap buffer (snprintf-return accumulation, int cap-pos underflow) | `src/stdlib/yaml.c:449` | CONFIRMED |
| 120.5 | PAR-02 | High | remote-unauth | toon_from_json single-object path uses a fixed 4096-byte buffer with no growth | `src/stdlib/toon.c:356` | REFUTED |
| 120.5 | PAR-03 | High | remote-unauth | JSON skip_string reads out of bounds on a trailing backslash | `src/stdlib/json.c:36` | CONFIRMED |
| 120.5 | PAR-04 | High | remote-unauth | Unbounded recursion in non-streaming JSON/YAML value skippers (stack-overflow DoS) | `src/stdlib/json.c:82` | CONFIRMED |
| 120.5 | PAR-05 | High | remote-unauth | md_render forces CMARK_OPT_UNSAFE and emits unsanitized link hrefs (XSS) | `src/stdlib/md.c:318` | REFUTED |
| 120.5 | PAR-06 | Medium | remote-unauth | toon_arr trusts declared [count] and returns uninitialized element pointers | `src/stdlib/toon.c:302` | — |
| 120.5 | PAR-07 | Medium | remote-unauth | TOON converters grow the buffer by a single doubling that can still be too small | `src/stdlib/toon.c:509` | — |
| 120.5 | PAR-08 | Medium | remote-unauth | md_inline_cell recurses without a depth bound (stack-overflow DoS) | `src/stdlib/md.c:80` | — |
| 120.5 | PAR-09 | Medium | remote-auth | Template engine: unbounded block/partial recursion and layout path traversal | `src/stdlib/template.c:795` | — |
| 120.5 | PAR-10 | Low | local | Missing allocation NULL-checks across parsers (DoS; csv gbuf_ensure short-write on failure) | `src/stdlib/csv.c:48` | — |
| 120.5 | PAR-11 | Low | remote-auth | soap_fault embeds the detail argument without XML-escaping (injection) | `src/stdlib/soap.c:171` | — |
| 120.6 | HTT-01 | High | remote-unauth | Chunked request bodies bypass max_body limit and can hang/exhaust a worker | `src/stdlib/http.c:324` | CONFIRMED |
| 120.6 | HTT-04 | High | remote-auth | ws_recv integer overflow: malloc(payload_len+1) heap overflow on client path | `src/stdlib/ws.c:986` | CONFIRMED |
| 120.6 | HTT-02 | Medium | remote-unauth | Buffered pipelined chunk bytes discarded, causing chunked desync/smuggling | `src/stdlib/http.c:878` | — |
| 120.6 | HTT-03 | Medium | remote-unauth | Request-smuggling hardening gaps in CL/TE header parsing | `src/stdlib/http.c:774` | — |
| 120.6 | HTT-05 | Low | remote-unauth | HTTP/2 frame and HPACK sizing bound to peer-advertised rather than local limits | `src/stdlib/http2.c:136` | — |
| 120.6 | HTT-06 | Low | remote-unauth | Slowloris: per-read timeout with no whole-request deadline against a fixed worker pool | `src/stdlib/http.c:743` | — |
| 120.6 | HTT-07 | Low | remote-unauth | Per-worker rate-limit state weakens the global limit | `src/stdlib/http.c:663` | — |
| 120.6 | HTT-08 | Low | remote-auth | HPACK decode error/OOM paths can NULL-deref and leak | `src/stdlib/http2.c:850` | — |
| 120.7 | WEB-01 | High | remote-unauth | Heap buffer overflow in reverse-proxy request builder (unbounded snprintf offset) | `src/stdlib/proxy.c:314` | CONFIRMED |
| 120.7 | WEB-02 | Medium | remote-unauth | http.servedir lacks symlink/allowed-root resolution (weaker than router_static_serve) | `src/stdlib/tk_web_glue.c:729` | — |
| 120.7 | WEB-03 | Low | remote-unauth | Reverse proxy forwards/duplicates client-controlled trust headers | `src/stdlib/proxy.c:309` | — |
| 120.7 | WEB-04 | Low | remote-unauth | Per-request memory leak of route param strings in dynamic handler dispatch | `src/stdlib/tk_web_glue.c:216` | — |
| 120.7 | WEB-05 | Low | remote-unauth | CORS wildcard + credentials reflects any origin credentialed | `src/stdlib/router.c:303` | — |
| 120.7 | WEB-06 | Info | remote-unauth | Inconsistent decoding: route params not percent-decoded; router query decoder keeps %00 | `src/stdlib/router.c:638` | — |
| 120.8 | TLS-01 | High | remote-unauth | Stack buffer overflow in x509_fingerprint_hex (fixed 8 KiB DER buffer) | `src/stdlib/tls.c:167` | CONFIRMED |
| 120.8 | TLS-02 | Medium | remote-unauth | tls_connect performs no server authentication by default; no system-CA trust path | `src/stdlib/tls.c:253` | — |
| 120.8 | TLS-03 | Medium | local | Connection-registry race enables use-after-free of SSL* | `src/stdlib/tls.c:686` | — |
| 120.8 | ACME-01 | Medium | local | snprintf length-accumulation overflow in identifiers[4096]/san_buf[4096] | `src/stdlib/acme.c:445` | — |
| 120.8 | ACME-02 | Medium | remote-unauth | HTTP-01 challenge store is a global mutated without synchronization and freed while the handler may read it | `src/stdlib/acme.c:810` | — |
| 120.8 | ACME-03 | Low | local | JSON injection via unescaped email/domain values in ACME payloads | `src/stdlib/acme.c:394` | — |
| 120.8 | ACME-04 | Low | local | Expired certificate is never renewed (days<0 conflates error and already-expired) | `src/stdlib/acme.c:915` | — |
| 120.8 | ACME-05 | Low | local | Predictable temp filename and missing O_EXCL on key/cert writes | `src/stdlib/acme.c:825` | — |
| 120.8 | TLS-06 | Low | local | Connection id embeds a live SSL*/heap pointer (ASLR leak if exposed) | `src/stdlib/tls.c:78` | — |
| 120.8 | ACME-07 | Info | local | Account key loaded from disk without permission check | `src/stdlib/acme.c:362` | — |
| 120.9 | CRY-01 | Medium | local | RNG fallback fails open to zero/uninitialised keys on non-Apple platforms | `src/stdlib/encrypt.c:43` | — |
| 120.9 | AUTH-01 | Medium | remote-unauth | auth_jwtverify does not enforce exp; expired tokens verify as valid | `src/stdlib/auth.c:392` | — |
| 120.9 | AUTH-02 | Low | remote-unauth | TOTP code comparison uses non-constant-time strcmp | `src/stdlib/auth.c:957` | — |
| 120.9 | ENC-02 | Low | remote-auth | RSA-OAEP decode is not constant-time (Manger-style padding-oracle shape) | `src/stdlib/encrypt.c:2509` | — |
| 120.9 | HYG-01 | Low | local | Sensitive key material left un-zeroized on stack/heap | `src/stdlib/encrypt.c:2151` | — |
| 120.10 | DAT-01 | High | remote-auth | Query builder emits string-concatenated SQL with no escaping (SQL injection) | `src/stdlib/db_glue.c:283` | CONFIRMED |
| 120.10 | DAT-05 | High | remote-auth | Query-builder INSERT/UPDATE can overflow the 4096-byte SQL buffer (heap overflow) | `src/stdlib/db_glue.c:289` | CONFIRMED |
| 120.10 | DAT-03 | High | build-time | MySQL backend silently discards bound parameters | `src/stdlib/db_mysql.c:123` | REFUTED |
| 120.10 | DAT-02 | Medium | remote-auth | Legacy raw-SQL wrappers accept a single pre-built SQL string with no params | `src/stdlib/db_glue.c:115` | — |
| 120.10 | DAT-04 | Medium | build-time | MySQL my_table_exists concatenates the table name into SQL | `src/stdlib/db_mysql.c:247` | — |
| 120.10 | DAT-06 | Medium | build-time | No TLS enforcement / plaintext-credential fallback for the MySQL backend | `src/stdlib/db_mysql.c:76` | — |
| 120.10 | DAT-07 | Low | remote-auth | Driver error strings propagated verbatim to the application | `src/stdlib/db.c:71` | — |
| 120.10 | DAT-08 | Low | local | exec_simple leaks the SQLite errmsg allocation and mishandles ownership | `src/stdlib/db.c:286` | — |
| 120.11 | AMB-01 | High | build-time | No capability gate: full ambient OS authority granted unconditionally | `src/stdlib/os.c:23` | CONFIRMED |
| 120.11 | AMB-02 | High | remote-auth | process.exec / spawndetached / readlines run untrusted strings through /bin/sh -c | `src/stdlib/process_glue.c:18` | REFUTED |
| 120.11 | AMB-03 | High | remote-auth | process.spawn resolves argv[0] via execvp (PATH-trusting) while PATH is program-writable | `src/stdlib/process.c:131` | REFUTED |
| 120.11 | AMB-04 | High | local | env_file_load sets arbitrary environment variables with no key allowlist | `src/stdlib/env.c:347` | REFUTED |
| 120.11 | AMB-05 | Medium | remote-auth | file_rmdir_r follows symlinks (stat not lstat) — recursive delete escapes the tree | `src/stdlib/file.c:249` | — |
| 120.11 | AMB-06 | Medium | remote-auth | path_join / file_join do not normalize '..', unsafe as containment primitives | `src/stdlib/path.c:21` | — |
| 120.11 | AMB-07 | Low | remote-auth | File open/read/write/copy follow symlinks (no O_NOFOLLOW); write-through-symlink and TOCTOU | `src/stdlib/file.c:76` | — |
| 120.11 | AMB-08 | Low | local | Raw os.read/os.write take a caller-supplied integer as the buffer address | `src/stdlib/os.c:31` | — |
| 120.11 | AMB-09 | Low | local | process_set_cwd is a no-op — spawned children never chdir | `src/stdlib/process.c:482` | — |
| 120.11 | AMB-10 | Low | local | Server binary-upgrade trusts TK_LISTEN_FD from the environment and clears CLOEXEC | `src/stdlib/server_ops.c:169` | — |
| 120.11 | AMB-11 | Info | local | Non-reentrant getcwd/strerror bridges (static buffers, no error signal) | `src/stdlib/os.c:71` | — |
| 120.12 | INF-01 | Medium | local | popen() shell command with only single-quote wrapping of statfs device name | `src/stdlib/infer_stream.c:205` | — |
| 120.12 | INF-02 | Medium | remote-auth | MLX bridge requests interpolate prompt/text/model_path without JSON escaping | `src/stdlib/mlx.c:498` | — |
| 120.12 | INF-03 | Medium | remote-auth | Model-API client has no TLS: Bearer key and traffic sent cleartext, no cert validation | `src/stdlib/llm.c:198` | — |
| 120.12 | INF-04 | Low | local | vecstore collection_load trusts count/dim from on-disk .vecs file | `src/stdlib/vecstore.c:281` | — |
| 120.12 | INF-05 | Low | local | vecstore replace-on-OOM leaves entry with NULL payload that later crashes save | `src/stdlib/vecstore.c:492` | — |
| 120.12 | INF-06 | Low | remote-auth | No host allowlist or CRLF guard on model-API host/path (SSRF surface) | `src/stdlib/llm.c:150` | — |
| 120.13 | RUN-01 | High | build-time | Entire WAF/rate-limit/SQLi/XSS engine is dead code: linked but never invoked and unreachable from toke | `src/stdlib/security.c:1` | CONFIRMED |
| 120.13 | RUN-02 | Medium | remote-unauth | Rate limiter and connection limiter fail OPEN when the 4096-bucket table is exhausted | `src/stdlib/security.c:76` | — |
| 120.13 | RUN-03 | Medium | remote-unauth | Pre-fork worker model multiplies rate limits and de-globalizes connection accounting | `src/stdlib/security.c:29` | — |
| 120.13 | RUN-04 | Medium | remote-unauth | Per-route composite rate-limit key is truncated to 63 bytes, causing bucket collisions | `src/stdlib/security.c:134` | — |
| 120.13 | RUN-05 | Medium | remote-unauth | SQL-injection heuristic is case-sensitive and trivially bypassable | `src/stdlib/security.c:282` | — |
| 120.13 | RUN-06 | Medium | remote-unauth | XSS heuristic is incomplete and matches pre-decode input | `src/stdlib/security.c:306` | — |
| 120.13 | RUN-07 | Medium | local | CSP builder can overflow its 2048-byte buffer via unchecked snprintf accumulation | `src/stdlib/security.c:461` | — |
| 120.13 | RUN-08 | Low | remote-unauth | WAF header matching NULL-derefs; REDIRECT action unusable; substring-only matching bypassable | `src/stdlib/security.c:387` | — |
| 120.13 | RUN-09 | Low | remote-unauth | Connection counter leaks and drifts, risking self-inflicted lockout | `src/stdlib/security.c:163` | — |
| 120.13 | RUN-10 | Low | remote-unauth | URI validator does not decode and misses traversal/double-encoding; permits raw TAB | `src/stdlib/security.c:214` | — |
| 120.13 | RUN-11 | Info | local | Process-wide CSP singleton and shared-global mutation in the route rate check | `src/stdlib/security.c:137` | — |
| 120.14 | OOK-01 | High | remote-unauth | Static file server exposes the entire project directory (source/config disclosure) | `src/serve.tk:153` | REFUTED |
| 120.14 | OOK-02 | High | remote-unauth | Templates are unescaped by default ({=x=}); HTML escaping is opt-in via \|escape | `src/template.tk:70` | REFUTED |
| 120.14 | OOK-03 | Medium | remote-unauth | No authn/authz/session/CSRF/rate-limit primitives; documented admin interface is unimplemented | `src/config.tk:24` | — |
| 120.14 | OOK-04 | Low | build-time | SQL table/column names built by string concatenation in the sqlite store | `src/store.tk:96` | — |
| 120.14 | OOK-05 | Info | remote-unauth | CORS origin string passed to setcors without validation | `src/serve.tk:130` | — |
| 120.14 | OOK-06 | Info | remote-unauth | Router path-parameter capture stored unescaped (latent reflected XSS) | `src/router.tk:102` | — |
| 120.15 | TOK-01 | High | remote-unauth | Rate limiting and connection limits silently fail open (advertised control non-functional) | `lib/redis-client.cjs:10` | CONFIRMED |
| 120.15 | TOK-02 | Medium | remote-unauth | Native tkc compiler executed with no resource isolation or sandbox | `tools/compile.js:23` | — |
| 120.15 | TOK-03 | Low | remote-unauth | Argument injection into tkc via preserve_atoms (no -- sentinel, no validation) | `tools/compress.js:29` | — |
| 120.15 | TOK-04 | Low | remote-unauth | tokeBench 'pro tier' authorization gate is a no-op | `lib/tier-gate.js:4` | — |
| 120.15 | TOK-05 | Info | remote-unauth | Wildcard CORS with credentialed Authorization header exposed | `server.js:291` | — |
| 120.16 | DEP-01 | Medium | local | SSH deploy disables host-key verification (StrictHostKeyChecking=no) | `toke-website/scripts/deploy.sh:45` | — |
| 120.16 | DEP-02 | Medium | remote-unauth | Production TLS is self-signed with no ACME/auto-renewal | `toke-website/main.tk:37` | — |
| 120.16 | DEP-03 | Medium | local | Unencrypted TLS private key rsynced to prod and kept in the working tree | `toke-website/scripts/deploy.sh:140` | — |
| 120.16 | DEP-04 | Low | local | No process supervision: pkill + nohup, no restart on crash/reboot | `toke-website/scripts/deploy.sh:227` | — |
| 120.16 | DEP-05 | Low | local | Compiler toolchain and C sources shipped to and compiled on the production host | `toke-website/scripts/deploy.sh:215` | — |
| 120.16 | DEP-06 | Low | build-time | Live infra IP and SSH key path hardcoded in deploy-script comments | `toke-website/scripts/deploy.sh:28` | — |
| 120.16 | DEP-07 | Medium | remote-auth | API keys stored in cleartext as the DynamoDB partition key (cross-reference) | `toke-cloud/lambda/api-gateway/toke_api_lambda.py:202` | — |
| 120.19 | REL-01 | High | remote-unauth | Chunked request bodies bypass max_body limit and overflow on crafted chunk size | `src/stdlib/http.c:376` | CONFIRMED |
| 120.19 | REL-02 | Medium | remote-unauth | Eager per-connection allocation of max_header+max_body amplifies memory | `src/stdlib/http.c:720` | — |
| 120.19 | REL-03 | Medium | local | process capture API deadlocks (write-all-stdin then read-stdout) | `src/stdlib/process.c:348` | — |
| 120.19 | REL-04 | Low | local | process_spawn ignores set_cloexec failure; parent can hang after successful exec | `src/stdlib/process.c:97` | — |
| 120.19 | REL-05 | Low | local | file_listall uses a non-reentrant static accumulator falsely labelled thread-local | `src/stdlib/file.c:619` | — |
| 120.19 | REL-06 | Low | build-time | arena_alloc uses signed int sizing; large requests overflow to heap overflow | `src/arena.c:77` | — |
| 120.19 | REL-07 | Low | remote-unauth | Pre-fork worker respawn has no backoff or crash-loop cap | `src/stdlib/http.c:1363` | — |
| 120.19 | REL-08 | Low | remote-unauth | Unchecked malloc/strdup on the request-parse path can NULL-deref under memory pressure | `src/stdlib/http.c:170` | — |

---

## Per-module reports

Each story maps to one per-module report in this directory.

| Story | Module | Report |
|---|---|---|
| 120.2 | Compiler toolchain invocation & command construction | [compiler-toolchain.md](compiler-toolchain.md) |
| 120.3 | Compiler front-end memory safety | [compiler-frontend.md](compiler-frontend.md) |
| 120.4 | Compiler IR / codegen memory safety | [compiler-codegen.md](compiler-codegen.md) |
| 120.5 | Data-format parser family | [parsers.md](parsers.md) |
| 120.6 | HTTP / protocol core | [http-core.md](http-core.md) |
| 120.7 | Web glue, routing, static serving & proxy | [web-glue.md](web-glue.md) |
| 120.8 | TLS / ACME / transport | [tls-acme.md](tls-acme.md) |
| 120.9 | Crypto & secrets | [crypto-secrets.md](crypto-secrets.md) |
| 120.10 | Database layer & injection | [database.md](database.md) |
| 120.11 | Ambient authority surface | [ambient-authority.md](ambient-authority.md) |
| 120.12 | LLM / inference stack & popen injection | [inference.md](inference.md) |
| 120.13 | In-runtime defense review (WAF layer) | [runtime-waf.md](runtime-waf.md) |
| 120.14 | toke-ooke app-layer audit | [ooke.md](ooke.md) |
| 120.15 | toke-mcp audit (public MCP server) | [toke-mcp.md](toke-mcp.md) |
| 120.16 | Website + cloud deployment posture | [deployment.md](deployment.md) |
| 120.19 | Error-handling & reliability guarantees review | [reliability.md](reliability.md) |

### Supporting reports (no per-finding rows)

| Story | Purpose | Report |
|---|---|---|
| — | Reconnaissance & scope | [recon.md](recon.md) |
| — | Audit overview / entry point | [README.md](README.md) |
| 120.17 | Secure-by-default design review | [design-review.md](design-review.md) |
| 120.22 | Fuzzing harnesses | [fuzzing.md](fuzzing.md) |
| 120.23 | Adversarial fuzz corpus | [adversarial-corpus.md](adversarial-corpus.md) |
| 120.24 | CI security checks + recurring dependency/secret scanning | [ci-security.md](ci-security.md) |

---

## Findings → Epic 121

Epic 120 is the **audit**; remediation is tracked in **Epic 121**. Each finding in
the [All findings](#all-findings) table becomes its own **121.N** remediation story,
filed and prioritized by the main thread (not by this index and not in this
directory). Suggested filing order:

1. **CONFIRMED High findings first** (16) — verified-exploitable, immediate priority.
2. **Remaining High findings** — REFUTED High findings (8) should be re-scoped or
   downgraded during triage before a 121.N story is opened.
3. **Medium**, then **Low**, then **Info**, weighting `remote-unauth` reachability
   highest within each tier.

The main thread owns creating the 121.N stories in `toke/docs/progress.md`; this
roll-up is the input inventory, not the tracker.

---

## Requires owner decision

Two cross-cutting findings cannot be resolved by a local code fix; they need an
architecture decision from the project owner before remediation stories can be
scoped. Both ADRs are currently **Proposed**.

> **ADR-0010 — Ambient authority (Proposed).**
> Drives Story 120.11 (AMB-01 *No capability gate: full ambient OS authority
> granted unconditionally*, High/CONFIRMED) and the surrounding file/process/env
> ambient-authority findings. Decision needed: whether toke adopts a capability
> gate / sandbox model for OS access, and the default posture (deny-by-default vs.
> current grant-all). Blocks the AMB-* remediation stories.

> **ADR-0011 — Injection / auto-escape (Proposed).**
> Drives the injection and output-encoding cluster: DAT-01/DAT-05 (SQL injection &
> SQL buffer overflow, High/CONFIRMED), the parser/template escaping findings
> (PAR-*), and the toke-ooke default-escaping findings (OOK-02). Decision needed:
> whether toke/ooke adopt auto-escaping and parameterized-query-by-default
> semantics, and the migration/compat story. Blocks the injection-class
> remediation stories.
