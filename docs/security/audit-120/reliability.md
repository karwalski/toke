## 120.19 — Error-handling & reliability guarantees review

Scope: runtime error/panic and resource-management paths in the toke C stdlib and
compiler arena — `src/stdlib/http.c` (server loop + pre-fork worker pool),
`src/stdlib/process.c`, `src/stdlib/file.c`, and `src/arena.c`. Method: manual
static reading of the actual source (no build, no dynamic execution). Focus:
error/panic propagation, resource-exhaustion caps (allocation/recursion),
fork-child crash isolation in the pre-fork server, partial-failure semantics,
and unchecked `malloc`/syscall return values. Every finding cites code that was
read directly.

---

### REL-01 — Chunked request bodies bypass `max_body` and overflow on crafted chunk size
- **Severity:** High
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:324-401` (`http_chunked_read`), call site `src/stdlib/http.c:875-884`

**Description.** The Content-Length request path enforces `srv_limits.max_body`
(`http.c:809-816`), but the chunked path does not. When a request carries
`Transfer-Encoding: chunked`, `handle_connection` calls `http_chunked_read(fd, …)`
(`http.c:878`), and that function has **no total-size ceiling** at all — it keeps
`realloc`-doubling `buf` for as many chunks as the client sends.

Worse, the per-chunk size is accumulated into a `size_t` with no bound:

```c
csz = csz * 16u + nibble;           // http.c:362, attacker-controlled hex, no cap
...
if (buf_len + csz + 1 > buf_cap) {  // http.c:376 — integer overflow when csz≈SIZE_MAX
    while (buf_cap < buf_len + csz + 1) buf_cap *= 2;
    ...
}
size_t rread = 0;
while (rread < csz) {               // http.c:385
    ssize_t n = read(fd, buf + buf_len + rread, csz - rread);
    ...
}
```

A client that sends a chunk-size line of `ffffffffffffffff` makes `csz == SIZE_MAX`.
Then `buf_len + csz + 1` wraps to a small value, the grow test is false, the buffer
stays 4096 bytes, and the subsequent `read()` loop writes attacker data past the
end of `buf` — a **heap buffer overflow**. Even without the exact overflow, a large
`csz` (e.g. 64 GiB) drives `buf_cap *= 2` until `realloc` fails, and an unbounded
stream of moderate chunks drives unbounded memory growth on a single connection —
all after the operator's `max_body` limit was supposed to bound the request.

Secondary correctness/desync defect on the same path: `handle_connection` reads
headers with `read(fd, …)` (`http.c:745`), which can also pull the first body
bytes into `raw`. `http_chunked_read` then starts reading fresh from `fd`, silently
dropping any pipelined leading chunk data and mis-framing the body.

**Impact.** Remote, unauthenticated heap overflow (memory corruption → worker
crash, potential RCE) and unbounded-memory DoS that ignores the configured body
limit. This is the most serious reliability/safety issue in scope.

**Fix.** Enforce `srv_limits.max_body` inside `http_chunked_read` (pass the limit
in; abort when `buf_len + csz` exceeds it). Reject any `csz` above the cap before
allocating/reading. Guard the size arithmetic against overflow
(`if (csz > cap - buf_len) fail;`). Feed already-buffered post-header bytes into the
decoder instead of discarding them.

---

### REL-02 — Eager per-connection allocation of `max_header + max_body` amplifies memory
- **Severity:** Medium
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:720-723`

**Description.** For every accepted socket, before a single body byte is read,
`handle_connection` allocates the full worst-case buffer:

```c
size_t buf_cap = (size_t)srv_limits.max_header
               + (size_t)srv_limits.max_body + 8;   // 8 KiB + 1 MiB default
char *raw = malloc(buf_cap);
```

With defaults (`HTTP_DEFAULT_MAX_BODY_SIZE = 1 MiB`, `http.h:168`) each live
connection immediately holds ~1 MiB resident even for a tiny `GET`. `http_set_limits`
accepts `max_body` up to `UINT32_MAX`, so an operator who raises the body limit to,
say, 256 MiB makes every idle keep-alive connection cost 256 MiB. A slowloris-style
client that opens many connections and sends only headers (kept alive for the idle
window, `KEEPALIVE_IDLE_TIMEOUT_S`) forces linear memory blow-up. Per-IP rate
limiting (`http.c:674`) caps connections per source IP but is trivially bypassed
with spoofed/rotated source addresses or a modest botnet.

**Impact.** Memory-exhaustion DoS disproportionate to actual request size.

**Fix.** Allocate the header buffer up front (8 KiB) and grow the body region lazily
only as body bytes actually arrive, capped at `max_body`.

---

### REL-03 — `process` capture API can deadlock (write-all-stdin then read-stdout)
- **Severity:** Medium
- **Reachability:** local
- **Location:** `src/stdlib/process.c:327-363` (`process_stdin_write`), `:248-304` (`process_stdout`)

**Description.** `process_stdin_write` loops until **all** `len` bytes are written to
the child's stdin pipe (`process.c:348-360`), and `process_stdout` is a separate
call that drains stdout afterwards. A child that emits output before consuming all
of its stdin will fill its stdout pipe buffer (~64 KiB), block on `write`, and stop
reading stdin; the parent then blocks forever in `write(h->stdin_fd, …)` because the
stdin pipe is full and the child never drains it. Classic pipe deadlock — both
sides hang with no timeout.

**Impact.** A toke program feeding a large payload to a filter-style subprocess
(the common `echo | grep`-style pattern) hangs indefinitely, wedging the calling
worker/task. Reliability failure with no recovery path.

**Fix.** Interleave stdin writes with stdout/stderr draining (poll/select over the
three fds), or document the payload-size constraint and set the pipe non-blocking
with a timeout.

---

### REL-04 — `process_spawn` ignores `set_cloexec` failure → parent can hang after successful exec
- **Severity:** Low
- **Reachability:** local
- **Location:** `src/stdlib/process.c:97`, `:157-162`

**Description.** The exec-failure detection idiom relies on `FD_CLOEXEC` being set on
the error-pipe write end so the kernel auto-closes it on a successful `execvp`,
making the parent's blocking `read(err_pipe[0], …)` return 0. The return value of
`set_cloexec(err_pipe[1])` is discarded (`process.c:97`). If `fcntl` fails, the write
end is inherited across `exec`, the exec'd program keeps it open, and the parent's
blocking `read` at `process.c:160` does not return until that program exits — i.e.
`process_spawn` blocks for the entire lifetime of the child instead of returning a
handle.

**Impact.** Rare (fcntl failure), but when it happens the spawn call hangs
indefinitely. `dup2` return values in the child (`process.c:116-127`) are likewise
unchecked; a failed `dup2` silently mis-wires the child's I/O.

**Fix.** Check `set_cloexec`'s return; on failure, close everything and return
`PROCESS_ERR_IO`. Check `dup2` in the child and `_exit` on failure.

---

### REL-05 — `file_listall` uses a non-reentrant static accumulator falsely labelled "thread-local"
- **Severity:** Low
- **Reachability:** local
- **Location:** `src/stdlib/file.c:618-656`

**Description.** `nftw` provides no user-data pointer, so `file_listall` accumulates
results into a **process-global** `static struct _listall_acc` (`file.c:619`) — the
comment calls it "thread-local accumulator" but there is no `_Thread_local`. Two
concurrent `file_listall` calls (from tasks/threads) clobber each other's `data`,
`len`, `cap`, and `base_len`, producing data races, lost/duplicated entries,
`realloc` on a pointer another thread already freed, and double-free on cleanup.

**Impact.** Heap corruption / crash under concurrent directory listing. The pre-fork
HTTP server isolates by process so single-worker use is safe, but any threaded/async
caller (`task.c`) is exposed.

**Fix.** Make the accumulator `_Thread_local`, or use `fts(3)`/an explicit stack walk
that carries context by pointer.

---

### REL-06 — `arena_alloc` uses `int` sizing; large allocation requests overflow → heap overflow
- **Severity:** Low
- **Reachability:** build-time
- **Location:** `src/arena.c:76-95`

**Description.** `arena_alloc(Arena*, int size)` computes
`aligned = (size + TKC_ARENA_ALIGN - 1) & ~(TKC_ARENA_ALIGN - 1)` and tests
`b->used + aligned > b->cap`, all in signed `int`. A request near `INT_MAX`
(e.g. a ~2 GiB identifier/string/array copy from a pathological source file) overflows
`aligned` to a negative value: the capacity test becomes false, no new block is
allocated, `b->used += aligned` corrupts the offset, and `memset(p, 0, (size_t)size)`
writes `size` bytes into an undersized block — heap overflow while compiling.

**Impact.** Compiler memory corruption when fed a hostile/huge source. Build-time
only, but a compiler that a CI system runs on untrusted code is exposed.

**Fix.** Type sizes as `size_t`, reject/So-clamp requests above a sane cap, and check
`aligned`/`used + aligned` for overflow before use.

---

### REL-07 — Pre-fork worker respawn has no backoff or crash-loop cap
- **Severity:** Low
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:1356-1378`

**Description.** When a worker exits abnormally the supervisor immediately `fork`s a
replacement (`http.c:1363-1375`) with no rate limiting, backoff, or restart-count
cap. A request that reliably crashes a worker (e.g. via REL-01) lets an attacker
drive continuous kill/respawn churn. If a worker were to crash *before* `accept`
(startup fault), the supervisor tight-loops forking with only the incidental
`nanosleep` on the WNOHANG branch as relief.

**Impact.** Sustained fork/respawn storm — CPU burn and log flooding — under a
crash-triggering request stream. Note the *isolation itself is a positive*: one
worker crash never takes down siblings or the supervisor.

**Fix.** Track per-slot restart timestamps; apply exponential backoff and give up
(or alert) after N restarts in a window.

---

### REL-08 — Unchecked `malloc`/`strdup` on the request-parse path → NULL-deref crash under memory pressure
- **Severity:** Low
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/http.c:170`, `:178-179`, `:186`

**Description.** `parse_request` allocates the header array and duplicates strings
without checking for failure: `StrPair *hdrs = malloc(64 * sizeof(StrPair));`
(`http.c:170`) is dereferenced at `hdrs[hc].key = strdup(p)` (`http.c:178`) with no
NULL check, and `req.method`/`req.path`/`req.body` come from unchecked `strdup`
(`http.c:167,169,186`). Under memory pressure any of these returns NULL and the
worker either dereferences NULL or later treats a NULL `method`/`path` as a valid
string.

**Impact.** Worker crash (mitigated by REL-07 respawn) when the process is near its
memory ceiling — which an attacker can help induce via REL-01/REL-02.

**Fix.** Check every allocation; on failure return an empty/zeroed `Req` and let the
caller emit 500.

---

### Dynamic-testing follow-up (needs runtime proof, not run here per audit rules)
- **REL-01:** Fuzz the chunked decoder under ASAN with a `ffffffffffffffff` chunk
  size and oversized/streamed chunks to confirm the heap overflow and the
  max_body bypass; also test header+body coalesced in one TCP segment to confirm the
  leading-byte desync.
- **REL-02:** Load test with N idle keep-alive connections while watching RSS to
  quantify the per-connection amplification at raised `max_body`.
- **REL-03:** Spawn a child that writes >64 KiB to stdout before reading stdin, then
  call `process_stdin_write` with a >64 KiB payload; confirm the deadlock.
- **REL-05:** Run `file_listall` from multiple threads under TSan/ASAN to confirm the
  race/double-free.
- **REL-06:** Compile a source that forces a >2 GiB arena request under ASAN.

### Positive observations (defenses already correct)
- **Fork-child crash isolation** in the pre-fork pool is sound: workers share no
  mutable state (COW route snapshot, `http.c:1088-1104`), and abnormal exits are
  detected and respawned (`http.c:1363-1375`). One worker crash cannot corrupt
  siblings or the supervisor.
- **Request-smuggling defence:** ambiguous `Content-Length` + `Transfer-Encoding:
  chunked` requests are rejected (`http.c:804-807`, and the TLS path `:3926-3929`).
- **process.c exec-failure detection** uses the race-free `FD_CLOEXEC` error-pipe
  idiom, and correctly reaps the child on exec failure and on handle-alloc failure
  (`process.c:164-197`); `EINTR` is retried on every blocking `read`/`write`/`waitpid`.
- **file.c error handling** is generally careful: syscall returns are checked, errno
  is saved before `fclose`, allocations are NULL-checked with full cleanup, and
  partial-list allocations free everything already accumulated (`file.c:545-558`,
  `:602-609`). `file_rmdir_r` avoids `system("rm -rf")` and propagates the first error.
- **Graceful shutdown/drain** with a timeout then `SIGKILL` fallback is implemented
  for both shutdown and SIGHUP reload (`http.c:1258-1345`).
- **arena_alloc/block_new** correctly propagate `malloc` failure as NULL rather than
  crashing (the only gap is the signed-int overflow of REL-06).
