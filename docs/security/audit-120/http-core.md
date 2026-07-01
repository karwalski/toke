## 120.6 — HTTP / protocol core (largest remote surface)

Static/manual review of the C HTTP stack that backs ooke: `src/stdlib/http.c` (pre-fork
server, request-line/header/Content-Length/chunked parsing, multipart, form, cookies),
`http2.c` (HTTP/2 framing + HPACK), `ws.c` / `ws_server.c` (WebSocket framing), and
`sse.c` (Server-Sent Events writer). Focus areas were request smuggling (CL/TE), byte-parser
bounds/integer bugs, resource limits, fork-child isolation, and WS frame length/masking.
No build was run and no dynamic testing was performed; every finding cites code that was
read directly. Line numbers refer to the files as they exist in the repo at audit time.

Reachability legend: `remote-unauth` = triggerable by any network client with no auth;
`remote-auth` = requires an authenticated/established peer or the app acting as a client.

---

### HTT-01 — Chunked request bodies bypass `max_body` and can hang/exhaust a worker
- **Severity:** High
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:324` (`http_chunked_read`), invoked at `src/stdlib/http.c:876-884`
- **Description:** The connection handler enforces `srv_limits.max_body` (default 1 MiB,
  `http.h:168`) only on the `Content-Length` path (`http.c:809-816`). When a request carries
  `Transfer-Encoding: chunked` (`http.c:786-801`), the body is instead reassembled by
  `http_chunked_read(fd, ...)` (`http.c:878`), which grows its output buffer with no ceiling:
  `while (buf_cap < buf_len + csz + 1) buf_cap *= 2;` then `realloc` (`http.c:376-381`). There
  is no check of `buf_len`/`csz` against `max_body` or any absolute cap. The per-chunk size
  `csz` is accumulated from attacker hex with no bound (`http.c:352-364`).
- **Impact:**
  1. **`max_body` bypass / memory exhaustion:** an attacker streams an arbitrarily large
     chunked body and forces unbounded heap growth in the worker process, ignoring the
     configured body limit.
  2. **Integer-overflow hang:** a single chunk-size line such as `fffffffffffffff0\r\n`
     yields a `csz` near `SIZE_MAX`. `buf_cap *= 2` keeps doubling; when `buf_cap` passes 2^63
     it wraps to 0 and the loop condition `buf_cap < huge` stays true forever → an infinite
     CPU-bound loop that pins the pre-fork worker at 100% and never returns. With a bounded
     worker pool this is a cheap remote DoS.
- **Recommended fix:** Enforce `max_body` inside `http_chunked_read` (pass the limit in and
  fail once `buf_len + csz` exceeds it), reject a `csz` that exceeds a sane per-chunk cap, and
  guard the `buf_cap` doubling against overflow (e.g. compute the needed size with an overflow
  check and fail closed rather than looping).

---

### HTT-02 — Buffered pipelined chunk bytes are discarded → chunked desync / smuggling
- **Severity:** Medium
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:739-764` (Phase-1 header read) and `src/stdlib/http.c:876-884`
- **Description:** Phase 1 reads from the socket until it *finds* `\r\n\r\n` (`http.c:763`).
  A single TCP segment frequently delivers the request headers *and* the first bytes of the
  chunked body together, so `raw` already contains part of the chunk stream past the header
  terminator. For the chunked path the handler throws those buffered bytes away and calls
  `http_chunked_read(fd, ...)` (`http.c:878`), which reads *fresh* from the socket starting
  after the already-consumed bytes. The decoder therefore begins mid-body, mis-parses the
  hex length line, and typically returns `NULL`; the original (wrong) body is kept and, when
  keep-alive is active, the leftover bytes on the socket are interpreted as the next request.
- **Impact:** Chunked bodies are silently corrupted, and on keep-alive connections the
  residual bytes desynchronise the request stream. Behind a connection-pooling reverse proxy
  this is a request-smuggling primitive (one client's trailing bytes are prefixed to another
  client's request on a reused backend connection).
- **Recommended fix:** Feed the bytes already present in `raw` after `\r\n\r\n` into the
  chunked decoder before reading more from the socket (make `http_chunked_read` accept an
  initial buffer), and on any chunked protocol error close the connection instead of
  continuing keep-alive.

---

### HTT-03 — Request-smuggling hardening gaps in CL/TE header parsing
- **Severity:** Medium
- **Reachability:** remote-unauth (cross-user impact requires an upstream proxy)
- **Location:** `src/stdlib/http.c:772-807`
- **Description:** The CL/TE handling has several parser-fidelity gaps that can desync this
  origin from an upstream proxy:
  - **Duplicate `Content-Length` not rejected:** the scan takes the *first* `Content-Length`
    and stops (`http.c:774-784`). RFC 7230 requires rejecting a message with conflicting
    duplicate CL values; a proxy that honours the last value desyncs from this server.
  - **Whitespace-obfuscated CL evades detection:** `strncasecmp(p, "Content-Length:", 15)`
    (`http.c:777`) requires the colon immediately after the name, so `Content-Length : 5`
    (space before colon) is *not* recognised as a body length. The origin then reads no body
    while a lenient proxy treats it as CL=5.
  - **TE detection is first-header / prefix only:** only the first `Transfer-Encoding` header
    is examined and the value must *start with* `chunked` (`http.c:788-800`), so
    `Transfer-Encoding: gzip, chunked`, a second TE header, or obfuscated casing/spacing is
    not treated as chunked here even though another hop might.
  - `atoll(cl_hdr)` (`http.c:810`) accepts trailing garbage (`5x`) and `+5`, another source
    of proxy/origin disagreement.
  The explicit CL+TE rejection at `http.c:803-807` is good but does not cover these cases.
- **Impact:** When ooke is deployed behind a reverse proxy/CDN (the common production shape),
  these discrepancies enable HTTP request smuggling (cache poisoning, auth bypass, request
  hijacking).
- **Recommended fix:** Reject any request with more than one `Content-Length`, with a CL
  value that is not a pure decimal string, or with any `Transfer-Encoding` present alongside
  `Content-Length`; normalise header-name matching to reject whitespace before the colon; and
  treat any `Transfer-Encoding` whose token list contains `chunked` (in any position) as
  chunked, rejecting `chunked` that is not the final coding.

---

### HTT-04 — `ws_recv` integer overflow: `malloc(payload_len + 1)` heap overflow (client path)
- **Severity:** High
- **Reachability:** remote-auth (toke program acting as a WebSocket *client* to a malicious server)
- **Location:** `src/stdlib/ws.c:986` (`ws_recv`); analogous `src/stdlib/ws.c:371,384` (`ws_decode_frame`)
- **Description:** `ws_recv` reads a peer-supplied 64-bit frame length into `payload_len`
  (`ws.c:960-973`) with **no maximum-frame-size check**, then allocates
  `malloc(payload_len + 1)` (`ws.c:986`). A length field of `0xFFFFFFFFFFFFFFFF` makes
  `payload_len + 1` wrap to `0`, so `malloc(0)` returns a minimal allocation; the code then
  calls `ws_recv_all(conn->fd, payload, payload_len)` (`ws.c:994`) and unmask writes
  (`ws.c:1004-1007`) against that undersized buffer → heap buffer overflow as soon as any
  payload bytes arrive. `ws_decode_frame` has the sibling bug: the guard
  `buflen < header_len + payload_len` (`ws.c:371`) can wrap when `payload_len` is near
  `UINT64_MAX`, and `malloc(payload_len + 1)` (`ws.c:384`) wraps the same way. Even without
  the wrap, the absence of any frame-size cap allows a malicious peer to request multi-GB
  allocations (memory-exhaustion DoS).
- **Impact:** Heap corruption / crash (and potential RCE) in any toke program that uses the
  `ws.connect`/`ws.recv` client API against an untrusted endpoint; at minimum a remote
  memory-exhaustion DoS. Note the *server-side* reader `ws_server_read_frame`
  (`ws_server.c:294-346`) is **not** affected — it enforces `g_ws_config.max_frame_size`
  (default 1 MiB, `ws_server.c:319`) before allocating.
- **Recommended fix:** In `ws_recv` and `ws_decode_frame`, reject `payload_len` above a
  configurable maximum before allocating, and compute allocation/comparison sizes with
  explicit overflow checks (e.g. `if (payload_len > SIZE_MAX - 1) fail;`). Apply the same
  frame-size cap the server path already uses.

---

### HTT-05 — HTTP/2 frame/HPACK sizing not bound to locally advertised limits
- **Severity:** Low
- **Reachability:** remote-unauth (via `h2c` cleartext upgrade, `http.c:851-873`, when built with OpenSSL)
- **Location:** `src/stdlib/http2.c:136` and `src/stdlib/http2.c:824-829`
- **Description:** Two RFC 9113 / 7541 limit-enforcement inversions:
  - `h2_frame_recv` bounds an incoming frame by `conn->peer_settings.max_frame_size`
    (`http2.c:136`) — the size the *peer* advertised for frames *it* receives — instead of
    our own advertised `local_settings.max_frame_size` (default 16384). Because a peer can
    legitimately raise `peer_settings.max_frame_size` up to `H2_MAX_FRAME_SIZE_LIMIT`
    (16 MiB, validated at `http2.c:242`), it can then send us a 16 MiB frame that we accept
    and `realloc` a per-connection buffer for (`http2.c:145-151`) — a ~1000× amplification
    over the 16 KiB we advertised.
  - The HPACK "dynamic table size update" (`http2.c:824-828`) calls
    `hpack_table_resize(t, new_size)` with an unvalidated attacker `new_size` (up to ~2^28
    from `hpack_decode_int`), never checking it against the connection's advertised
    `SETTINGS_HEADER_TABLE_SIZE`. RFC 7541 §6.3 requires treating an oversize update as a
    decoding error.
- **Impact:** Per-connection memory amplification / limit bypass. Bounded by data the attacker
  must actually send, so DoS-grade rather than corruption, but it defeats the configured caps.
- **Recommended fix:** Bound incoming frames by `local_settings.max_frame_size`; reject a
  dynamic-table-size update that exceeds the advertised header-table size.

---

### HTT-06 — Slowloris: per-read timeout, no whole-request deadline, fixed worker pool
- **Severity:** Low
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:710-764`
- **Description:** The only time limit is `SO_RCVTIMEO` (default 10 s, `http.h:169`), which
  resets on every `read` that returns ≥1 byte (`http.c:743-761`). A client that trickles one
  header byte every ~9 s keeps a pre-fork worker occupied for up to
  `max_header × timeout` (≈ 8192 × 10 s) with no whole-request wall-clock cap. With a bounded
  worker pool (`http_serve_workers`), a handful of slow connections can occupy every worker.
- **Impact:** Classic slowloris connection-exhaustion DoS.
- **Recommended fix:** Add an absolute per-request/per-connection deadline (monotonic clock),
  and/or cap concurrent connections per source IP; consider a non-blocking accept loop.

---

### HTT-07 — Per-worker rate-limit state weakens the global limit
- **Severity:** Low
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:651-691` (`g_rate_table`, `rate_check`)
- **Description:** The token buckets live in process-global `g_rate_table` (`http.c:663`).
  After `fork()` each worker gets its own copy-on-write copy, so the effective per-IP limit is
  `RATE_LIMIT_MAX × nworkers` and depends on which worker happens to `accept` the connection;
  a respawned worker resets its counters. Additionally, bucket selection is a plain FNV hash
  into 1024 slots (`http.c:665-672`) and a collision is treated as a new IP that *resets* the
  bucket (`http.c:679-689`), so two IPs hashing to the same slot repeatedly clear each other's
  counts.
- **Impact:** The advertised 200 req/60 s per-IP limit is substantially weaker than intended
  and unevenly enforced; it is a soft speed bump, not a reliable control.
- **Recommended fix:** Share rate-limit state across workers (shared memory / atomic ops) or
  document it as best-effort; use per-IP buckets that don't reset on hash collision.

---

### HTT-08 — Minor robustness issues in HPACK/OOM error paths
- **Severity:** Low
- **Reachability:** remote-auth (HTTP/2 peer, only under allocation failure)
- **Location:** `src/stdlib/http2.c:848-870`
- **Description:** In `hpack_decode`, a failed `realloc` overwrites `names`/`values` with
  `NULL` while leaking the old buffer (`http2.c:850-852`); the subsequent `goto err` then does
  `for (...) free(names[i])` (`http2.c:864-866`), dereferencing the now-`NULL` array →
  crash. `strdup` results at `http2.c:808-809` are not NULL-checked, and per-field `name`/
  `value` allocated in the current iteration are leaked on several `goto err` paths
  (`http2.c:822,844`). These require allocation failure to trigger.
- **Impact:** Under memory pressure, a NULL-deref crash rather than a clean error. Not
  attacker-steerable beyond inducing OOM.
- **Recommended fix:** Use temporary pointers for `realloc`, check every allocation, and free
  the in-flight `name`/`value` before `goto err`.

---

## Dynamic-testing follow-up (needs runtime proof, not run here)
- Fuzz `http_chunked_read` under ASAN with oversized/near-`SIZE_MAX` chunk-size lines to
  confirm the HTT-01 infinite loop and unbounded-growth behaviour, and to measure real memory
  ceilings.
- Fuzz `ws_recv` / `ws_decode_frame` (HTT-04) with a length field of `0xFFFFFFFFFFFFFFFF`
  followed by a few payload bytes, under ASAN, to confirm the `malloc(0)` heap overflow.
- Stand up ooke behind nginx/HAProxy and replay the HTT-02/HTT-03 desync/smuggling payloads
  (duplicate CL, `Content-Length :`, `TE: gzip, chunked`, pipelined chunk bytes) to confirm
  cross-request contamination on pooled backend connections.
- Load-test the pre-fork pool with trickle clients (HTT-06) to quantify worker exhaustion.

## Positive observations (defenses already correct)
- The `Content-Length` path enforces `max_body` and rejects negative/oversize lengths
  (`http.c:809-816`), and the fixed-CL body read is correctly bounded by `buf_cap`
  (`http.c:825-839`) — no overflow there.
- Ambiguous `Content-Length` + `Transfer-Encoding: chunked` is explicitly rejected with 400
  (`http.c:803-807`) — the core smuggling case is handled.
- Header block is size-capped (`max_header`, 431 on overflow, `http.c:743-770`).
- The server-side WebSocket reader enforces a configurable `max_frame_size` (default 1 MiB)
  *before* allocating (`ws_server.c:318-334`), unlike the client path.
- The HPACK static table is correctly 1-indexed with a padding entry, so
  `hpack_lookup` cannot read out of bounds (`http2.c:462-527, 613-627`); Huffman output is
  allocated at 2× (`http2.c:743`), safely above the 8/5 worst-case expansion; `hpack_decode_int`
  has overflow protection (`http2.c:650`).
- HTTP/2 `MAX_FRAME_SIZE`/`INITIAL_WINDOW_SIZE`/`ENABLE_PUSH` settings values are range-checked
  (`http2.c:222-244`), and SETTINGS payloads must be a multiple of 6 (`http2.c:208`).
- Multipart parsing bounds total body (50 MiB) and per-part (10 MiB) sizes and uses
  length-bounded `mp_find`/`memcmp` throughout (`http.c:1609-1875`).
- `url_decode` rejects `%00` and never expands output beyond input (`http.c:1405-1420`).
- The worker supervisor uses `nanosleep` yields rather than busy-spinning
  (`http.c:1347-1353`).
