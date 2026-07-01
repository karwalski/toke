## 120.12 — LLM / inference stack & popen injection

**Scope / method.** Static, read-only review of the toke inference stdlib:
`src/stdlib/infer_stream.c`, `infer.c`, `llm.c`, `mlx.c`, `vecstore.c`, plus the
i64-ABI glue that wires these into compiled toke programs
(`infer_glue.c`, `mlx_glue.c`, `vecstore_glue.c`, `tk_web_glue.c`). Risk classes
examined: shell/`popen` command injection with interpolated device paths, JSON
injection into local model bridges, transport security / SSRF / cert validation
on outbound model-API calls, model-file and vector-index trust boundaries, and
memory-safety on untrusted parsing paths. No build or dynamic tooling was run.

**Reachability note (important for triage).** The shipped glue for `std.infer`
and `std.mlx` (`infer_glue.c`, `mlx_glue.c`) are **stubs** — `tk_infer_generate_w`,
`tk_mlx_generate_w`, etc. return fixed sentinels and never call the real
`infer.c` / `mlx.c` / `infer_stream.c` code. So the injection sinks in those
files are **latent**: not reachable from a normally-compiled toke program today,
but live the moment the real implementations are wired (e.g. a `TK_HAVE_LLAMACPP`
build or a future non-stub glue). `vecstore.c` **is** live via `vecstore_glue.c`,
and `llm.c` **is** live via `tk_web_glue.c` (`tk_llm_complete_w` / `tk_llm_chat_w`).
Reachability tags below reflect this.

---

### INF-01 — `popen()` runs a shell with only single-quote wrapping of a device name

- **Severity:** Medium
- **Reachability:** local (currently latent — sink gated behind stubbed infer glue and `__APPLE__`)
- **Location:** `src/stdlib/infer_stream.c:201-205` (`detect_macos`)

**Description.** Storage-type detection on macOS shells out:

```c
char cmd[TK_STREAM_PATH_MAX + 64];
snprintf(cmd, sizeof(cmd), "diskutil info '%s' 2>/dev/null", sfs.f_mntfromname);
FILE *pipe = popen(cmd, "r");
```

`sfs.f_mntfromname` is the mount source string returned by `statfs()` for the
filesystem containing `model_dir`. The value is wrapped in single quotes, but
single-quoting does **not** neutralise an embedded single quote: a
`f_mntfromname` of `x'; touch /tmp/pwned; '` closes the quote, injects a command,
and reopens it. `f_mntfromname` is attacker-influenceable for network and
image-backed mounts (SMB/AFP/NFS share names, attached `.dmg`/sparsebundle
volume names), which can contain arbitrary characters. A victim who mounts such
a share and then runs a toke program that calls `infer.load_streaming` on a path
under that mount would execute the injected command with the program's full
ambient authority (there is no language sandbox).

**Impact.** Arbitrary command execution in the context of the toke process if a
crafted mount-source name reaches `detect_macos`. Currently gated behind the
stubbed infer glue, so not reachable from stock builds — but this is the exact
class the story targets and should be fixed before the real glue lands.

**Recommended fix.** Do not build a shell string. Use `posix_spawn`/`fork`+`execvp`
with an argv array (`{"diskutil","info",dev,NULL}`) and read the child's stdout
via a pipe, so `dev` is passed as a single literal argument and never parsed by a
shell. Alternatively use the IOKit `IOServiceGetMatchingServices` /
"Protocol Characteristics" path referenced in the file comment, avoiding a
subprocess entirely.

---

### INF-02 — MLX bridge requests interpolate prompt/text/model_path without JSON escaping

- **Severity:** Medium
- **Reachability:** remote-auth / local (currently latent — `mlx_glue.c` is a stub)
- **Location:** `src/stdlib/mlx.c:409`, `mlx.c:497-499`, `mlx.c:563`

**Description.** The MLX client builds request bodies with raw `%s`:

```c
snprintf(req, req_cap, "{\"model_path\":\"%s\"}", model_path);                    // :409
snprintf(req, req_cap, "{\"id\":\"%s\",\"prompt\":\"%s\",\"max_tokens\":%d}",     // :497
         m->id, prompt, (int)max_tokens);
snprintf(req, req_cap, "{\"id\":\"%s\",\"text\":\"%s\"}", m->id, text);           // :563
```

None of `prompt`, `text`, or `model_path` is JSON-escaped (contrast `llm.c`,
which routes all user content through `json_escape`). A `prompt` value containing
`"` / `\` / control characters breaks out of the JSON string. Because the body is
sent to the local bridge with a correct `Content-Length` (`strlen`), this is not
HTTP request smuggling — but it **is** JSON injection into the bridge: a prompt
like `hi","max_tokens":999999,"x":"` injects/overrides sibling fields, and any
field the bridge honours (sampling params, model path, tool config) becomes
attacker-controllable when the prompt is user-supplied (e.g. an ooke chat
endpoint). Malformed input can also simply DoS the bridge parse.

**Impact.** Parameter override / field injection into the local MLX bridge from
untrusted prompt/text; magnitude depends on the bridge's JSON handling.

**Recommended fix.** Escape every interpolated string with the existing
`json_escape` helper (or a shared one) before placing it in the body, and size
`req_cap` for the escaped length. Buffers `req_cap = id_len + prompt_len + 64`
also assume no expansion and must grow to the escaped size.

---

### INF-03 — Outbound model-API client has no TLS: Bearer key and traffic sent in cleartext, no cert validation

- **Severity:** Medium
- **Reachability:** remote-auth (live via `tk_llm_complete_w` / `tk_llm_chat_w`)
- **Location:** `src/stdlib/llm.c:8-11`, `llm.c:194-213`, `llm.c:506-508` (and the parallel `https` rejects at 592, 968, 1035, 1179, 1344)

**Description.** `llm.c` speaks only plaintext HTTP/1.1 over raw sockets; TLS is
not implemented, and every entry point explicitly **rejects** `https://`:

```c
if (c->base_url && strncmp(c->base_url, "https://", 8) == 0)
    return err_resp("HTTPS requires TLS support (not compiled in); use http://...");
```

The request writes `Authorization: Bearer <api_key>` in the clear
(`llm.c:198`). The default endpoint (`http://localhost:11434/v1`, `tk_web_glue.c:1978`)
is local and fine, but any operator who points `LLM_BASE_URL` at a remote
provider is forced onto `http://` and therefore transmits the API key and all
prompt/response content over an unauthenticated, unencrypted channel. There is
no certificate validation because there is no TLS at all: an on-path attacker can
read the API key and, more subtly, **inject a forged model response** that then
flows back into the application (and, in agentic/tool setups, into `tk_tool_call_w`),
i.e. a response-integrity / prompt-injection vector.

**Impact.** Credential disclosure and response tampering for any remote model
endpoint. Confined to loopback in the default configuration.

**Recommended fix.** Implement TLS (e.g. OpenSSL/BoringSSL BIO or mbedTLS) with
certificate + hostname verification for `https://` endpoints, and refuse to send
an `Authorization` header over a non-loopback `http://` connection (fail closed,
or require an explicit opt-in flag). Until TLS exists, document that remote
providers are unsupported and keep the client loopback-only.

---

### INF-04 — vecstore trusts `count`/`dim` from the on-disk `.vecs` file

- **Severity:** Low
- **Reachability:** local (live via `vecstore_glue.c` → `vecstore_collection`)
- **Location:** `src/stdlib/vecstore.c:252-254`, `vecstore.c:281`, `vecstore.c:283`

**Description.** `collection_load` reads a 4-byte `count` and `dim` straight from
the file header and uses them to drive allocation:

```c
int32_t new_cap = (int32_t)count;              // count is u32 from file
...
VecEntry *entries = calloc((size_t)new_cap, sizeof(VecEntry));   // :254
...
emb = (float *)malloc(sizeof(float) * dim);    // :281, dim u32 from file
```

A hostile or corrupt `.vecs` file with a large `count` (e.g. 100M) forces a large
`calloc` and a read loop that only stops when `fread` fails — a memory-exhaustion
DoS. On a 64-bit target `sizeof(float) * dim` cannot overflow `size_t`, but on a
32-bit build `dim` near `UINT32_MAX` overflows the product to a small allocation
followed by a `dim`-element `fread` — a heap overflow. In an ooke deployment the
`data_dir` may hold attacker-supplied collection files (upload, shared volume),
crossing the trust boundary.

**Impact.** Denial of service (memory) on all targets; heap overflow on 32-bit
builds only.

**Recommended fix.** Sanity-cap `count` and `dim` against a configured maximum
and against the actual remaining file size (`stat`/`ftell`) before allocating;
use overflow-checked size math (e.g. reject `dim > SIZE_MAX/sizeof(float)`).

---

### INF-05 — vecstore replace-on-OOM leaves an entry with a NULL payload that later crashes save

- **Severity:** Low
- **Reachability:** local (OOM-only)
- **Location:** `src/stdlib/vecstore.c:487-497`, crash site `vecstore.c:211`

**Description.** In the in-place replace branch of `vecstore_upsert`:

```c
free(col->entries[i].embedding);
free(col->entries[i].payload);
col->entries[i].embedding  = normed;
...
col->entries[i].payload    = malloc(strlen(payload) + 1);
if (!col->entries[i].payload) {
    col->entries[i].embedding = NULL;   // leaks `normed`
    return 0;
}
```

On payload-`malloc` failure the function leaks `normed` and leaves the entry in
the collection with `embedding == NULL` and `payload == NULL` (count unchanged).
`vecstore_search` guards `!e->embedding` (`vecstore.c:614`), but `collection_save`
does not: `uint32_t pay_len = (uint32_t)strlen(e->payload);` (`vecstore.c:211`)
dereferences the NULL payload on the next save/close, crashing the process.

**Impact.** Memory leak plus a later NULL-dereference crash, reachable only under
allocation failure.

**Recommended fix.** On the failure path, restore or delete the entry atomically
(don't free the old data until the new allocations succeed), free `normed`, and
never leave an entry with NULL `embedding`/`payload` in the array.

---

### INF-06 — No host allowlist / CRLF guard on model-API host & path (SSRF surface)

- **Severity:** Low
- **Reachability:** remote-auth (config-driven; `base_url` from env in current glue)
- **Location:** `src/stdlib/llm.c:143-173` (`tcp_connect`), `llm.c:81-135` (`parse_url`), `llm.c:195-213`

**Description.** `tcp_connect` resolves and connects to whatever host/port
`parse_url` extracts from `base_url`, with no restriction to loopback or an
allowlist, and the request builder interpolates `host`/`path`/`api_key` into the
header block without rejecting CR/LF. In the shipped glue `base_url` and
`api_key` come from environment variables (`tk_web_glue.c:1975-1977`), so this is
operator-controlled and low-risk today. However, any application that derives the
model endpoint (or a path segment) from untrusted input inherits an SSRF /
header-injection primitive: a `base_url` host pointing at internal
infrastructure, or a value containing `\r\n`, would let the caller reach internal
services or inject headers.

**Impact.** SSRF to internal endpoints and HTTP header injection **if** endpoint
configuration is ever attacker-influenced. Not exploitable through the current
env-only wiring.

**Recommended fix.** Reject hosts/paths/keys containing control characters; offer
an allowlist / "loopback-only" mode; and document that `base_url` must be treated
as trusted operator configuration, never derived from request data.

---

### Dynamic-testing follow-up

- **INF-01:** once real infer glue is wired, fuzz `f_mntfromname` handling by
  mounting volumes / shares whose names contain `'`, `;`, `` ` ``, `$()` and
  confirm no shell evaluation (or, after the fix, prove argv passing).
- **INF-02:** fuzz `mlx_generate`/`mlx_embed` with prompts containing `"`, `\`,
  `\n`, `}` and a mock bridge that logs the parsed JSON to confirm field
  injection is closed after escaping.
- **INF-04:** run `collection_load` under ASAN with hand-crafted `.vecs` headers
  (huge `count`, `dim` near `UINT32_MAX`, truncated bodies) on both 64-bit and
  32-bit builds to confirm bounds and detect the overflow path.
- **INF-05:** fault-inject `malloc` failure in the `vecstore_upsert` replace
  branch, then call `vecstore_close`/save, under ASAN, to confirm the crash and
  validate the fix.
- **INF-03:** stand up an on-path proxy and verify (post-fix) that TLS cert and
  hostname validation reject a mismatched certificate and that no Bearer header
  is emitted over non-loopback HTTP.

### Positive observations

- `vecstore_collection` rejects collection names containing `/` or `\\`
  (`vecstore.c:378`), preventing path traversal / absolute-path escape when
  building `{data_dir}/{name}.vecs`; a `..` name without a separator cannot
  traverse.
- `llm.c` routes all user-supplied message content and embedding text through
  `json_escape` before interpolation (`llm.c:322-341`, `979-985`, `1062-1066`),
  correctly closing the JSON-injection hole that `mlx.c` leaves open, and the
  body is length-delimited so no HTTP smuggling arises from content.
- HTTPS is fail-closed (rejected rather than silently downgraded) everywhere in
  `llm.c`, so no accidental plaintext fallback of an `https://` URL occurs.
- `parse_url`, `json_escape`, `find_json_string`, and the `http_post`/streaming
  read loops use explicit capacity checks and `snprintf` return-value guards;
  growth is bounded by `NUL`/`cap` and no fixed-buffer overflow was found in the
  HTTP request assembly (`req_buf[8192]` guarded at `llm.c:215`, `634`;
  `mlx.c:162`, `220`).
- `infer.c` generate/embed loops bound the output buffer with a realloc guard
  (`infer.c:259-268`) and serialise per-handle access with a mutex.
