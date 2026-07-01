## 120.7 — Web glue, routing, static serving & proxy

Scope: the native C behind ooke's HTTP surface — `src/stdlib/tk_web_glue.c`
(static-file directory serving, dynamic route dispatch, vhost catch-all),
`router.c` (route matching, param extraction, URL decoding, CORS middleware,
`router_static_serve` hardened static handler), `proxy.c` (reverse-proxy /
load-balancer), `net.c` / `net_glue.c` (raw TCP), and `mdns.c` (service
discovery). Method: manual source review with cross-referencing of the request
path from `http.c` (raw, un-decoded — `http.c:169`) through the two distinct
static-serving code paths and the proxy request builder. No build was run; a
few items are flagged for dynamic confirmation at the end.

Headline: the `http.servedir` handler uses a weaker traversal check than the
hardened `router_static_serve` and can be escaped via symlinks; the reverse
proxy's request builder has an unbounded-offset `snprintf` pattern that is a
heap overflow — though the proxy is not currently wired to any toke-callable
API. `../` path traversal itself is blocked on every path that was reviewed.

---

### WEB-01 — Heap buffer overflow in proxy request builder (unbounded `snprintf` offset)
- **Severity:** High
- **Reachability:** remote-unauth *conditional* — see exposure note
- **File:line:** `src/stdlib/proxy.c:290-331` (loop at 309-316; `buf[off]` at 331)

**Description.** `proxy_build_request` allocates a fixed 4096-byte buffer and
appends the request line, `Host`, forwarding headers, then every non-hop-by-hop
client header via the idiom:

```c
off += snprintf(buf + off, cap - (size_t)off, "%s: %s\r\n", name, val);
```

`snprintf` returns the number of bytes it *would* have written, so once the
accumulated `off` reaches/exceeds `cap` (4096), `off` keeps growing past the
buffer. On the next iteration `buf + off` points beyond the allocation and
`cap - (size_t)off` underflows (unsigned) to a near-`SIZE_MAX` limit, so
`snprintf` performs an out-of-bounds heap write. The terminating
`buf[off] = '\0'` (line 331) is likewise an OOB write when `off > cap`. The
header set is fully attacker-controlled in a reverse-proxy deployment (many
headers, or a single large header value such as a long `Cookie`/`Referer`).

**Impact.** Remote heap corruption → potential RCE or crash/DoS, triggered by
oversized or numerous request headers. There is no clamp of `off` to `cap` and
no per-append length guard.

**Exposure note (important).** `proxy_forward` / `proxy_route_handler` /
`proxy_upstream_new` are defined only in `proxy.c`; no header declares them and
no glue wrapper in `tk_web_glue.c` or `router_glue.c` exposes them to toke code
(the only proxy-related glue is `tk_http_withproxy_w`, which is the *outbound*
client proxy, a different mechanism). So the reverse proxy is currently latent /
unreachable from toke programs. The defect is real and should be fixed before
the feature is wired up; effective risk today is low because it is not on any
live request path.

**Fix.** Clamp `off` after each `snprintf` (`if (off >= (int)cap) { off = cap-1;
break; }` / grow the buffer), or rebuild using a bounds-checked append helper.
Reject requests whose serialized header block exceeds the buffer instead of
silently overrunning.

---

### WEB-02 — `http.servedir` lacks symlink/allowed-root resolution (weaker than `router_static_serve`)
- **Severity:** Medium
- **Reachability:** remote-unauth
- **File:line:** `src/stdlib/tk_web_glue.c:723-776` (check at 729-733, `fopen` at 742)

**Description.** `staticdir_handler` (the native handler behind `http.servedir`)
builds `filepath = g_staticdir_root + req.path` and its *only* traversal defence
is `strstr(filepath, "..")`. It then `stat()`s and `fopen()`s the path directly.
Unlike the hardened `router_static_serve` (`router.c:1251-1269`), it does **not**
call `realpath()` and does **not** verify the resolved path stays under the
configured root. Because `req.path` arrives un-decoded (`http.c:169`), literal
`../` is caught by the `strstr` and percent-encoded `%2e%2e%2f` never decodes
into `..`, so classic `../` traversal is blocked — but a **symlink inside the
served root that points outside it is followed silently**. E.g. a served tree
containing `link -> /etc` yields `GET /prefix/link/passwd` →
`fopen("<root>/link/passwd")` → `/etc/passwd`.

**Impact.** Disclosure of files outside the intended document root whenever the
served directory contains (or an uploads/build step introduces) a symlink.
Also, `staticdir_handler` applies no filtering of dotfiles or source files, so
serving a broad directory exposes `.git`, `.env`, `*.tk` sources, etc. — this is
the same class of concern as ooke's `servedir(projectdir)` usage tracked in
120.14; the two should be resolved together.

**Fix.** Mirror `router_static_serve`: `realpath()` both the root and the target
and reject when the resolved target is not prefixed by the resolved root (with a
`/` or end-of-string boundary). Consider routing `http.servedir` through
`router_static_serve` so there is a single hardened static path.

---

### WEB-03 — Reverse proxy forwards/duplicates client-controlled trust headers
- **Severity:** Low (Medium if proxy is exposed and a backend trusts these headers)
- **Reachability:** remote-unauth *conditional* (same non-exposure caveat as WEB-01)
- **File:line:** `src/stdlib/proxy.c:302-323`

**Description.** `proxy_build_request` writes its own
`X-Forwarded-For`/`X-Forwarded-Proto` (lines 303-306), then copies **all**
non-hop-by-hop client headers (lines 309-316), skipping only `Host`. It does not
strip an inbound client-supplied `X-Forwarded-For`, `X-Forwarded-Proto`,
`X-Real-IP`, or `Forwarded`, so a client can inject/override the apparent source
IP the backend sees. Separately, `Content-Length` is not in the hop-by-hop list,
so a client `Content-Length` is forwarded **and** the proxy appends its own
`Content-Length` (line 321) — producing a duplicate `Content-Length`, and
`X-Forwarded-Proto: https` is emitted even for plain-HTTP inbound connections.

**Impact.** IP-based trust/ACL bypass at the backend via spoofed `X-Forwarded-*`;
duplicate `Content-Length` is a request-smuggling/desync primitive with lenient
backends. Latent today (proxy not wired to toke).

**Fix.** Strip inbound `X-Forwarded-*`/`Forwarded`/`X-Real-IP` before re-adding
canonical values; exclude client `Content-Length`/`Transfer-Encoding` from the
forwarded set and set exactly one `Content-Length`; derive `X-Forwarded-Proto`
from the actual inbound scheme.

---

### WEB-04 — Per-request memory leak of route param strings (dynamic handlers)
- **Severity:** Low
- **Reachability:** remote-unauth
- **File:line:** `src/stdlib/tk_web_glue.c:201-224` (`tk_match_pattern`), `240-276` (`tk_get_handler_dispatch`)

**Description.** `tk_match_pattern` `malloc`s a key and value string for every
`:param` segment into the caller's `out[]` array. On a **successful** match,
`tk_get_handler_dispatch` uses them, frees `heap_req`, and returns without ever
freeing the param `key`/`val` strings (they are stack-array `StrPair`s pointing
at heap buffers that go out of scope). On a **partial** match that later fails
(`return 0` at line 219), the params already allocated before the mismatch are
likewise leaked (the caller just resets `pc = 0`). The same pattern is used by
the POST/PUT/DELETE/PATCH handler dispatchers.

**Impact.** Unbounded heap growth proportional to request volume on any server
using dynamic `:param` routes → gradual memory exhaustion / DoS on a
long-running process. Not a corruption bug.

**Fix.** Free `out[i].key` / `out[i].val` for all populated entries after the
handler returns, and on the mismatch path in `tk_match_pattern`.

---

### WEB-05 — CORS wildcard + credentials reflects any origin credentialed
- **Severity:** Low
- **Reachability:** remote-unauth (config-dependent)
- **File:line:** `src/stdlib/router.c:298-311` (`cors_match_origin`), `397-400`

**Description.** When `allowed_origins` contains `"*"` **and** `allow_credentials`
is set, `cors_match_origin` returns the caller's actual `Origin` (line 303-304)
rather than `"*"`, and the middleware then also emits
`Access-Control-Allow-Credentials: true` (lines 397-400). This is the classic
unsafe CORS combination: it reflects *any* origin while allowing credentials,
defeating the browser same-origin protection for credentialed requests.

**Impact.** If an app enables this (permitted, non-obvious) combination,
attacker origins can read authenticated cross-origin responses. Library footgun
rather than a fixed-config vuln.

**Fix.** Reject or warn on the `"*" + credentials` combination; require an
explicit origin allowlist whenever credentials are enabled.

---

### WEB-06 — Inconsistent decoding: params not percent-decoded; query decoder keeps `%00`
- **Severity:** Info / Low
- **Reachability:** remote-unauth (impact depends on app usage)
- **File:line:** `router.c:148-149` and `tk_web_glue.c:212-215` (params raw); `router.c:626-649` (`url_decode` keeps `%00`)

**Description.** Route `:slug`/`:param` values are handed to handlers **without**
percent-decoding (`match_route` → `strdup(req_segs[i])`, and `tk_match_pattern`
copies raw bytes). This is safe-by-default (no decoded `../`, no injected NUL)
but surprising: apps that decode a slug themselves and then use it in a
filesystem/SQL path re-introduce traversal/injection. Separately, `router.c`'s
`url_decode` (used by `router_query_get`) emits a NUL byte for `%00`
(line 638-639), whereas `http.c` (`http.c:1414`) and `encoding.c` deliberately
**drop** `%00` per the 57.4.3 path-traversal hardening — so a query value can
carry an embedded NUL and silently truncate if later used as a C-string path.

**Impact.** No direct native vulnerability, but an inconsistency that can defeat
the documented null-byte hardening at the application layer. Documentation +
consistency fix.

**Fix.** Decide and document whether params are decoded; if decoded, run the
same `%00`-dropping decoder used elsewhere. Align `router.c:url_decode` with the
`%00`-drop behaviour of `http.c`/`encoding.c`.

---

### Positive observations (defences already correct)
- `router_static_serve` (`router.c:1223-1269`) is well hardened:
  `has_traversal` on the relative path, `snprintf` overflow check, embedded-NUL
  rejection via `memchr`, and — crucially — `realpath()` of both root and target
  with a boundary-checked prefix comparison that blocks symlink escape. The
  vhost catch-all (`tk_web_glue.c:1006`) and `servepages` route through this
  path. WEB-02 is specifically about the *other* (`servedir`) handler not doing
  the same.
- The request path is not decoded before routing (`http.c:169`), so
  percent-encoded traversal never materialises into `..` on the filesystem;
  combined with the literal-`..` checks this blocks classic `../` traversal on
  both static paths.
- Reverse-proxy backends are fixed by application config
  (`proxy_upstream_add`, `proxy.c:437`); the request target host/port are **not**
  taken from the client, so there is no arbitrary-target SSRF in the proxy.
- `proxy_parse_response` bounds its read to a 64 KiB cap and treats the backend
  as the (trusted) config-defined host.
- `net_portavailable` (`net.c`) binds only `127.0.0.1` and validates the port
  range; `tk_net_listen_w` validates host via `inet_pton` and port range.
- `mdns.c` TXT parsing uses the platform DNS-SD library's bounded
  `TXTRecordGetItemAtIndex`/`TXTRecordGetCount` with fixed 256-byte key buffers
  and length-checked `memcpy`s; no obvious overflow, and the surface is
  link-local multicast rather than the public internet.
- Vhost selection uses the `Host` header only to pick a pre-registered docroot
  (`tk_web_glue.c:981-989`) with a safe default; it is not used to build
  filesystem paths, avoiding Host-header path injection.

### Dynamic-testing follow-up (needs runtime / ASAN, not run here)
- **WEB-01:** Build the (currently latent) reverse proxy and drive
  `proxy_build_request` under ASAN with a request carrying header bytes summing
  to >4096 to confirm the heap OOB write and buffer-underflow limit.
- **WEB-02:** Runtime test `http.servedir` against a served tree containing a
  symlink pointing outside the root to confirm the escape and validate a
  `realpath` fix.
- **WEB-03:** With a real backend, confirm duplicate `Content-Length` handling
  and `X-Forwarded-For` spoofing end-to-end.
- **WEB-04:** Long-running load test of a `:param` route watching RSS to confirm
  the leak rate.
- Fuzz `router_parse_request` / `http.c` request-line parsing and the CORS
  header path with malformed input for completeness.
